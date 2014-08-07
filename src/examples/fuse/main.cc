/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>
#include <memory>
#include <mutex>

#include <boost/make_shared.hpp>
#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_opt.h>
#include <polonaise/Polonaise.h>
#include <polonaise/polonaise_constants.h>
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TTransportUtils.h>

using namespace polonaise;

// A pool of Thrift Clients (ie. a pool of connections with some server)
template <typename Client>
class ThriftClientPool {
public:
	// Behaves like a Thrift Client (see operator->), but returns itself to a
	// client pool when destroyed.
	class Entry {
	public:
		Entry(ThriftClientPool& pool, std::unique_ptr<Client> client)
				: pool_(pool),
				  client_(std::move(client)) {
		}

		Entry(Entry&& other)
				: pool_(other.pool_),
				  client_(std::move(other.client_)) {
		}

		~Entry() {
			if (client_ != nullptr) {
				pool_.put(std::move(client_));
			}
		}

		// Marks client as unusable (eg. after losing a connection).
		// Prevents it from being returned to the pool.
		void markAsUnusable() {
			client_.reset();
		}

		Client* operator->() {
			return client_.get();
		}

	private:
		std::unique_ptr<Client> client_;
		ThriftClientPool& pool_;
	};

	ThriftClientPool(int maxSize) : maxSize_(maxSize), host_("localhost"), port_(9090) { }

	void setHost(std::string host) {
		host_ = std::move(host);
	}

	void setPort(int port) {
		port_ = port;
	}

	// Adds a new client to the pool. It will be returned back on destruction.
	void put(std::unique_ptr<Client> client) {
		std::unique_lock<std::mutex> lock(mutex_);
		clients_.push_back(std::move(client));
	}

	// Gets a client from the pool or creates a new client if the pool is empty
	Entry get() {
		using namespace apache::thrift;
		using namespace apache::thrift::protocol;
		using namespace apache::thrift::transport;

		std::unique_lock<std::mutex> lock(mutex_);
		while (clients_.size() > maxSize_) {
			clients_.pop_front();
		}
		if (clients_.empty()) {
			lock.unlock();
			// No clients in the pool; create a new connection
			fprintf(stderr, "Opening a new connection to %s:%d\n", host_.c_str(), port_);
			auto socket = boost::make_shared<TSocket>(host_, port_);
			auto transport = boost::make_shared<TBufferedTransport>(socket, 512 * 1024, 4096);
			auto protocol  = boost::make_shared<TBinaryProtocol>(transport);
			auto client = std::unique_ptr<Client>(new Client(protocol));
			socket->setNoDelay(true);
			transport->open();
			return Entry(*this, std::move(client));
		}
		// Take the first one form the pool
		Entry entry(*this, std::move(clients_.front()));
		clients_.pop_front();
		return entry;
	}

private:
	int maxSize_;
	std::string host_;
	int port_;
	std::mutex mutex_;
	std::list<std::unique_ptr<Client>> clients_;
};

ThriftClientPool<PolonaiseClient> gClientPool(30);
static SessionId gSessionId;

// Obtains session ID. Returns true iff successful.
bool polonaise_init() {
	try {
		gSessionId = gClientPool.get()->initSession();
		return true;
	} catch (apache::thrift::TException& tx) {
		fprintf(stderr, "ERROR: %s\n", tx.what());
		return false;
	}
}

void polonaise_term() {
}

// FUSE to Polonaise conversions

Descriptor get_descriptor(struct fuse_file_info *fi) {
	return fi ? static_cast<Descriptor>(fi->fh) : g_polonaise_constants.kNullDescriptor;
}

Context get_context(fuse_req_t& req) {
	const fuse_ctx *ctx = fuse_req_ctx(req);
	Context ret;
	ret.sessionId = gSessionId;
	ret.uid = ctx->uid;
	ret.gid = ctx->gid;
	ret.pid = ctx->pid;
	ret.umask = ctx->umask;
	return ret;
}

int get_open_flags(int posixFlags) {
	static std::pair<int, int> mappings[] = {
			{O_CREAT,  OpenFlags::kCreate},
			{O_EXCL,   OpenFlags::kExclusive},
			{O_APPEND, OpenFlags::kAppend},
			{O_TRUNC,  OpenFlags::kTrunc},
	};
	int polonaiseFlags = 0;
	for (const auto& mapping : mappings) {
		if (posixFlags & mapping.first) {
			polonaiseFlags |= mapping.second;
		}
	}
	switch (posixFlags & O_ACCMODE) {
		case O_RDONLY:
			polonaiseFlags |= OpenFlags::kRead;
			break;
		case O_WRONLY:
			polonaiseFlags |= OpenFlags::kWrite;
			break;
		case O_RDWR:
			polonaiseFlags |= (OpenFlags::kRead | OpenFlags::kWrite);
			break;
	}
	return polonaiseFlags;
}

int get_access_mask(int posixMask) {
	static std::pair<int, int> mappings[] = {
			{001, AccessMask::kExecute},
			{002, AccessMask::kWrite},
			{004, AccessMask::kRead},
	};
	int polonaiseMask = 0;
	for (const auto& mapping : mappings) {
		if (posixMask & mapping.first) {
			polonaiseMask |= mapping.second;
		}
	}
	return polonaiseMask;
}

Mode get_mode(int posixMode) {
	Mode result;
	result.ownerMask = get_access_mask((posixMode >> 6) & 07);
	result.groupMask = get_access_mask((posixMode >> 3) & 07);
	result.otherMask = get_access_mask((posixMode >> 0) & 07);
	result.sticky = (posixMode & S_ISVTX);
	result.setUid = (posixMode & S_ISUID);
	result.setGid = (posixMode & S_ISGID);
	return result;
}

FileType::type get_file_type(int posixMode) {
	if (S_ISREG(posixMode)) {
		return FileType::kRegular;
	}
	if (S_ISDIR(posixMode)) {
		return FileType::kDirectory;
	}
	if (S_ISLNK(posixMode)) {
		return FileType::kSymlink;
	}
	if (S_ISFIFO(posixMode)) {
		return FileType::kFifo;
	}
	if (S_ISBLK(posixMode)) {
		return FileType::kBlockDevice;
	}
	if (S_ISCHR(posixMode)) {
		return FileType::kCharDevice;
	}
	if (S_ISSOCK(posixMode)) {
		return FileType::kSocket;
	}
	Failure failure;
	failure.message = "client: Unknown file type in get_file_type";
	throw failure;
}

// Polonaise to FUSE conversions

int make_mode(const Mode& mode) {
	int ret = 0;
	ret |= (get_access_mask(mode.otherMask) << 0);
	ret |= (get_access_mask(mode.groupMask) << 3);
	ret |= (get_access_mask(mode.ownerMask) << 6);
	ret |= (mode.sticky ? S_ISVTX : 0);
	ret |= (mode.setUid ? S_ISUID : 0);
	ret |= (mode.setGid ? S_ISGID : 0);
	return ret;
}

struct stat make_stbuf(const FileStat& fs) {
	struct stat stbuf{};
	switch (fs.type) {
		case FileType::kRegular:
			stbuf.st_mode = S_IFREG;
			break;
		case FileType::kDirectory:
			stbuf.st_mode = S_IFDIR;
			break;
		case FileType::kSymlink:
			stbuf.st_mode = S_IFLNK;
			break;
		case FileType::kFifo:
			stbuf.st_mode = S_IFIFO;
			break;
		case FileType::kBlockDevice:
			stbuf.st_mode = S_IFBLK;
			break;
		case FileType::kCharDevice:
			stbuf.st_mode = S_IFCHR;
			break;
		case FileType::kSocket:
			stbuf.st_mode = S_IFSOCK;
			break;
	}
	stbuf.st_dev = fs.dev;
	stbuf.st_ino = fs.inode;
	stbuf.st_nlink = fs.nlink;
	stbuf.st_mode |= make_mode(fs.mode);
	stbuf.st_uid = fs.uid;
	stbuf.st_gid = fs.gid;
	stbuf.st_rdev = fs.rdev;
	stbuf.st_size = fs.size;
	stbuf.st_blocks = fs.blocks;
	stbuf.st_blksize = fs.blockSize;
	stbuf.st_atime = fs.atime;
	stbuf.st_mtime = fs.mtime;
	stbuf.st_ctime = fs.ctime;
#ifdef HAVE_STRUCT_STAT_ST_BIRTHTIME
	stbuf.st_birthtime = stbuf.st_ctime;
#endif
	return stbuf;
}

fuse_entry_param make_fuse_entry_param(const EntryReply& entry) {
	fuse_entry_param fep{};
	fep.ino           = entry.inode;
	fep.generation    = entry.generation;
	fep.attr          = make_stbuf(entry.attributes);
	fep.attr_timeout  = entry.attributesTimeout;
	fep.entry_timeout = entry.entryTimeout;
	return fep;
}

int make_error_number(StatusCode::type status) {
	switch(status) {
		case StatusCode::kE2BIG: return E2BIG;
		case StatusCode::kEACCES: return EACCES;
		case StatusCode::kEAGAIN: return EAGAIN;
		case StatusCode::kEBADF: return EBADF;
		case StatusCode::kEBUSY: return EBUSY;
		case StatusCode::kECHILD: return ECHILD;
		case StatusCode::kEDOM: return EDOM;
		case StatusCode::kEEXIST: return EEXIST;
		case StatusCode::kEFAULT: return EFAULT;
		case StatusCode::kEFBIG: return EFBIG;
		case StatusCode::kEINTR: return EINTR;
		case StatusCode::kEINVAL: return EINVAL;
		case StatusCode::kEIO: return EIO;
		case StatusCode::kEISDIR: return EISDIR;
		case StatusCode::kEMFILE: return EMFILE;
		case StatusCode::kEMLINK: return EMLINK;
		case StatusCode::kENAMETOOLONG: return ENAMETOOLONG;
		case StatusCode::kENFILE: return ENFILE;
		case StatusCode::kENODATA: return ENODATA;
		case StatusCode::kENODEV: return ENODEV;
		case StatusCode::kENOENT: return ENOENT;
		case StatusCode::kENOEXEC: return ENOEXEC;
		case StatusCode::kENOMEM: return ENOMEM;
		case StatusCode::kENOSPC: return ENOSPC;
		case StatusCode::kENOSYS: return ENOSYS;
		case StatusCode::kENOTBLK: return ENOTBLK;
		case StatusCode::kENOTDIR: return ENOTDIR;
		case StatusCode::kENOTEMPTY: return ENOTEMPTY;
		case StatusCode::kENOTTY: return ENOTTY;
		case StatusCode::kENXIO: return ENXIO;
		case StatusCode::kEPERM: return EPERM;
		case StatusCode::kEPIPE: return EPIPE;
		case StatusCode::kERANGE: return ERANGE;
		case StatusCode::kEROFS: return EROFS;
		case StatusCode::kESPIPE: return ESPIPE;
		case StatusCode::kESRCH: return ESRCH;
		case StatusCode::kETIMEDOUT: return ETIMEDOUT;
		case StatusCode::kETXTBSY: return ETXTBSY;
		case StatusCode::kEXDEV: return EXDEV;
		case StatusCode::kEDQUOT: return EDQUOT;
	}
	return EIO;
}

// Implementation of FUSE callbacks

#define HANDLE_POLONAISE_EXCEPTION_BEGIN \
	try { \
		auto polonaiseClient = gClientPool.get(); \
		try {

#define HANDLE_POLONAISE_EXCEPTION_END \
		} catch (Status& s) { \
			fuse_reply_err(req, make_error_number(s.statusCode)); \
		} catch (Failure& f) { \
			fprintf(stderr, "(%s) failure: %s\n", __FUNCTION__, f.message.c_str()); \
			fuse_reply_err(req, EIO); \
		} catch (apache::thrift::TException& e) { \
			fprintf(stderr, "(%s) connection error: %s\n", __FUNCTION__, e.what()); \
			fuse_reply_err(req, EIO); \
			polonaiseClient.markAsUnusable(); \
		} \
	} catch (apache::thrift::TException& e) /* gClientPool.get() may throw this */ { \
		fprintf(stderr, "(%s) connection error: %s\n", __FUNCTION__, e.what()); \
		fuse_reply_err(req, EIO); \
	}


void do_init(void *userdata, struct fuse_conn_info *conn) {
	conn->want |= FUSE_CAP_DONT_MASK;
}

void do_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	AttributesReply reply;
	polonaiseClient->getattr(reply, get_context(req), ino, get_descriptor(fi));
	const struct stat stbuf = make_stbuf(reply.attributes);
	fuse_reply_attr(req, &stbuf, reply.attributesTimeout);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	Descriptor descriptor = polonaiseClient->opendir(get_context(req), ino);
	fi->fh = static_cast<uint64_t>(descriptor);
	fuse_reply_open(req, fi);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
		struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	std::vector<DirectoryEntry> reply;
	polonaiseClient->readdir(reply, get_context(req), ino, off, 1 + size / 32, get_descriptor(fi));
	char buffer[65536];
	if (size > 65536) {
		size = 65536;
	}
	size_t written = 0;
	for (const auto& entry : reply) {
		const struct stat stbuf = make_stbuf(entry.attributes);
		size_t entrySize = fuse_add_direntry(req, buffer + written, size,
				entry.name.c_str(), &stbuf, entry.nextEntryOffset);
		if (entrySize > size) {
			break;
		}
		written += entrySize;
		size -= entrySize;
	}
	fuse_reply_buf(req, buffer, written);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	polonaiseClient->releasedir(get_context(req), ino, get_descriptor(fi));
	fi->fh = static_cast<uint64_t>(-2);
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	EntryReply reply;
	polonaiseClient->lookup(reply, get_context(req), parent, name);
	const fuse_entry_param fep = make_fuse_entry_param(reply);
	fuse_reply_entry(req, &fep);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_access(fuse_req_t req, fuse_ino_t ino, int mask) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	polonaiseClient->access(get_context(req), ino, get_access_mask(mask));
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	OpenReply reply;
	polonaiseClient->open(reply, get_context(req), ino, get_open_flags(fi->flags));
	fi->direct_io = reply.directIo;
	fi->keep_cache = reply.keepCache;
	fi->nonseekable = reply.nonSeekable;
	fi->fh = static_cast<uint64_t>(reply.descriptor);
	if (fuse_reply_open(req, fi) == -ENONET) {
		polonaiseClient->release(get_context(req), ino, reply.descriptor);
		fi->fh = static_cast<uint64_t>(-2);
	}

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	std::string buffer;
	polonaiseClient->read(buffer, get_context(req), ino, off, size, get_descriptor(fi));
	fuse_reply_buf(req, buffer.data(), buffer.size());

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	polonaiseClient->flush(get_context(req), ino, get_descriptor(fi));
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	polonaiseClient->release(get_context(req), ino, get_descriptor(fi));
	fi->fh = static_cast<uint64_t>(-2);
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_statfs(fuse_req_t req, fuse_ino_t ino) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	StatFsReply reply;
	polonaiseClient->statfs(reply, get_context(req), ino);
	struct statvfs s;
	s.f_fsid = reply.filesystemId;
	s.f_namemax = reply.maxNameLength;
	s.f_frsize = s.f_bsize = reply.blockSize;
	s.f_blocks = reply.totalBlocks;
	s.f_bfree = reply.freeBlocks;
	s.f_bavail = reply.availableBlocks;
	s.f_files = reply.totalFiles;
	s.f_ffree = reply.freeFiles;
	s.f_favail = reply.availableFiles;
	fuse_reply_statfs(req, &s);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_symlink(fuse_req_t req, const char *link, fuse_ino_t parent, const char *name) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	EntryReply reply;
	polonaiseClient->symlink(reply, get_context(req), link, parent, name);
	const fuse_entry_param fep = make_fuse_entry_param(reply);
	fuse_reply_entry(req, &fep);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_readlink(fuse_req_t req, fuse_ino_t ino) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	std::string reply;
	polonaiseClient->readlink(reply, get_context(req), ino);
	fuse_reply_readlink(req, reply.c_str());

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size, off_t off, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	std::string data(buf, size);
	int64_t count = polonaiseClient->write(get_context(req), ino, off, size, data, get_descriptor(fi));
	fuse_reply_write(req, count);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_fsync(fuse_req_t req, fuse_ino_t ino, int datasync, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	polonaiseClient->fsync(get_context(req), ino, datasync, get_descriptor(fi));
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr, int to_set, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	int32_t toSet = 0;
	toSet |= (to_set & FUSE_SET_ATTR_MODE)      ? ToSet::kMode     : 0;
	toSet |= (to_set & FUSE_SET_ATTR_UID)       ? ToSet::kUid      : 0;
	toSet |= (to_set & FUSE_SET_ATTR_GID)       ? ToSet::kGid      : 0;
	toSet |= (to_set & FUSE_SET_ATTR_SIZE)      ? ToSet::kSize     : 0;
	toSet |= (to_set & FUSE_SET_ATTR_ATIME)     ? ToSet::kAtime    : 0;
	toSet |= (to_set & FUSE_SET_ATTR_MTIME)     ? ToSet::kMtime    : 0;
	toSet |= (to_set & FUSE_SET_ATTR_ATIME_NOW) ? ToSet::kAtimeNow : 0;
	toSet |= (to_set & FUSE_SET_ATTR_MTIME_NOW) ? ToSet::kMtimeNow : 0;

	FileStat attributes;
	attributes.type = FileType::kFifo; // anything, server has to ignore it anyway
	if (toSet & ToSet::kMode) {
		attributes.mode = get_mode(attr->st_mode);
	}
	attributes.uid = (toSet & ToSet::kUid) ? attr->st_uid : 0;
	attributes.gid = (toSet & ToSet::kGid) ? attr->st_gid : 0;
	attributes.size = (toSet & ToSet::kSize) ? attr->st_size : 0;
	attributes.atime = (toSet & ToSet::kAtime) ? attr->st_atime : 0;
	attributes.mtime = (toSet & ToSet::kMtime) ? attr->st_mtime : 0;

	AttributesReply reply;
	polonaiseClient->setattr(reply, get_context(req), ino, attributes, toSet, get_descriptor(fi));
	const struct stat stbuf = make_stbuf(reply.attributes);
	fuse_reply_attr(req, &stbuf, reply.attributesTimeout);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_create(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	CreateReply reply;
	polonaiseClient->create(reply, get_context(req), parent, name, get_mode(mode), get_open_flags(fi->flags));
	const fuse_entry_param fep = make_fuse_entry_param(reply.entry);
	fi->fh = static_cast<uint64_t>(reply.descriptor);
	fi->direct_io = reply.directIo;
	fi->keep_cache = reply.keepCache;
	fuse_reply_create(req, &fep, fi);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_mknod(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode, dev_t rdev) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	EntryReply reply;
	polonaiseClient->mknod(reply, get_context(req), parent, name, get_file_type(mode), get_mode(mode), rdev);
	const fuse_entry_param fep = make_fuse_entry_param(reply);
	fuse_reply_entry(req, &fep);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	EntryReply reply;
	polonaiseClient->mkdir(reply, get_context(req), parent, name, FileType::kDirectory, get_mode(mode));
	const fuse_entry_param fep = make_fuse_entry_param(reply);
	fuse_reply_entry(req, &fep);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	polonaiseClient->rmdir(get_context(req), parent, name);
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char *newname) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	EntryReply reply;
	polonaiseClient->link(reply, get_context(req), ino, newparent, newname);
	const fuse_entry_param fep = make_fuse_entry_param(reply);
	fuse_reply_entry(req, &fep);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_unlink(fuse_req_t req, fuse_ino_t parent, const char *name) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	polonaiseClient->unlink(get_context(req), parent, name);
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_rename(fuse_req_t req, fuse_ino_t parent, const char *name, fuse_ino_t newparent, const char *newname) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	polonaiseClient->rename(get_context(req), parent, name, newparent, newname);
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

int main(int argc, char** argv) {
	gClientPool.setHost(getenv("POLONAISE_SERVER") ? getenv("POLONAISE_SERVER") : "localhost");
	gClientPool.setPort(getenv("POLONAISE_PORT") ? atoi(getenv("POLONAISE_PORT")) : 9090);
	if (!polonaise_init()) {
		fprintf(stderr, "Server connection failed.\n");
		return 1;
	}
	struct fuse_lowlevel_ops ops{};
	ops.init = do_init;
	ops.getattr = do_getattr;
	ops.opendir = do_opendir;
	ops.readdir = do_readdir;
	ops.releasedir = do_releasedir;
	ops.lookup = do_lookup;
	ops.access = do_access;
	ops.open = do_open;
	ops.read = do_read;
	ops.flush = do_flush;
	ops.release = do_release;
	ops.statfs = do_statfs;
	ops.symlink = do_symlink;
	ops.readlink = do_readlink;
	ops.write = do_write;
	ops.fsync = do_fsync;
	ops.setattr = do_setattr;
	ops.create = do_create;
	ops.mknod = do_mknod;
	ops.mkdir = do_mkdir;
	ops.rmdir = do_rmdir;
	ops.link = do_link;
	ops.unlink = do_unlink;
	ops.rename = do_rename;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	char *mp;
	int mt;
	if (fuse_parse_cmdline(&args, &mp, &mt, NULL) < 0) {
		perror("fuse_parse_cmdline");
		return 1;
	}
	fuse_chan *channel = fuse_mount(mp, &args);
	if (!channel) {
		perror("fuse_mount");
		return 1;
	}
	fuse_session *session = fuse_lowlevel_new(&args, &ops, sizeof(ops), NULL);
	if (!session) {
		perror("fuse_lowlevel_new");
		return 1;
	}
	fuse_session_add_chan(session, channel);
	fuse_set_signal_handlers(session);
	if (mt) {
		fuse_session_loop_mt(session);
	} else {
		fuse_session_loop(session);
	}
	fuse_remove_signal_handlers(session);
	return 0;
}
