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

#include <boost/make_shared.hpp>

#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_opt.h>

#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/transport/TSocket.h>
#include <thrift/transport/TTransportUtils.h>

#include <polonaise/Polonaise.h>
#include <polonaise/polonaise_constants.h>

using namespace polonaise;

static std::string gHostname;
static int gPort;
static boost::shared_ptr<apache::thrift::transport::TTransport> gThriftTransport;
static boost::shared_ptr<PolonaiseClient> gPolonaiseClient;
static SessionId gSessionId;

bool polonaise_init() {
	using namespace apache::thrift;
	using namespace apache::thrift::protocol;
	using namespace apache::thrift::transport;

	try {
		boost::shared_ptr<TTransport> socket(new TSocket(gHostname, gPort));
		gThriftTransport = boost::make_shared<TBufferedTransport>(socket);
		boost::shared_ptr<TProtocol> protocol(new TBinaryProtocol(gThriftTransport));
		gPolonaiseClient = boost::make_shared<PolonaiseClient>(protocol);
		gThriftTransport->open();
		gSessionId = gPolonaiseClient->initSession();
		return true;
	} catch (apache::thrift::TException &tx) {
		fprintf(stderr, "ERROR: %s\n", tx.what());
		return false;
	}
}

void polonaise_term() {
	try {
		gThriftTransport->close();
	} catch (apache::thrift::TException &tx) {
		fprintf(stderr, "ERROR: %s\n", tx.what());
	}
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
			{O_RDWR,   OpenFlags::kRead |  OpenFlags::kWrite},
			{O_RDONLY, OpenFlags::kRead},
			{O_WRONLY, OpenFlags::kWrite},
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
	stbuf.st_mtime = fs.ctime;
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
	}
	return EIO;
}

// Implementation of FUSE callbacks

#define HANDLE_POLONAISE_EXCEPTION_BEGIN try {
#define HANDLE_POLONAISE_EXCEPTION_END \
	} catch (Status& s) { \
		fuse_reply_err(req, make_error_number(s.statusCode)); \
	} catch (Failure& f) { \
		fprintf(stderr, "failure: %s\n", f.message.c_str()); \
		fuse_reply_err(req, EIO); \
	} catch (apache::thrift::TException& e) { \
		fprintf(stderr, "connection error: %s\n", e.what()); \
		fuse_reply_err(req, EIO); \
		polonaise_init(); \
	}

void do_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	AttributesReply reply;
	gPolonaiseClient->getattr(reply, get_context(req), ino, get_descriptor(fi));
	const struct stat stbuf = make_stbuf(reply.attributes);
	fuse_reply_attr(req, &stbuf, reply.attributesTimeout);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	Descriptor descriptor = gPolonaiseClient->opendir(get_context(req), ino);
	fi->fh = static_cast<uint64_t>(descriptor);
	fuse_reply_open(req, fi);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
		struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	std::vector<DirectoryEntry> reply;
	gPolonaiseClient->readdir(reply, get_context(req), ino, off, 1 + size / 32, get_descriptor(fi));
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

	gPolonaiseClient->releasedir(get_context(req), ino, get_descriptor(fi));
	fi->fh = static_cast<uint64_t>(-2);
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	EntryReply reply;
	gPolonaiseClient->lookup(reply, get_context(req), parent, name);
	const fuse_entry_param fep = make_fuse_entry_param(reply);
	fuse_reply_entry(req, &fep);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_access(fuse_req_t req, fuse_ino_t ino, int mask) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	gPolonaiseClient->access(get_context(req), ino, get_access_mask(mask));
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	OpenReply reply;
	gPolonaiseClient->open(reply, get_context(req), ino, get_open_flags(fi->flags));
	fi->direct_io = reply.directIo;
	fi->keep_cache = reply.keepCache;
	fi->nonseekable = reply.nonSeekable;
	fi->fh = static_cast<uint64_t>(reply.descriptor);
	if (fuse_reply_open(req, fi) == -ENONET) {
		gPolonaiseClient->release(get_context(req), ino, reply.descriptor);
		fi->fh = static_cast<uint64_t>(-2);
	}

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	std::string buffer;
	gPolonaiseClient->read(buffer, get_context(req), ino, off, size, get_descriptor(fi));
	fuse_reply_buf(req, buffer.data(), buffer.size());

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_flush(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	gPolonaiseClient->flush(get_context(req), ino, get_descriptor(fi));
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	gPolonaiseClient->release(get_context(req), ino, get_descriptor(fi));
	fi->fh = static_cast<uint64_t>(-2);
	fuse_reply_err(req, 0);

	HANDLE_POLONAISE_EXCEPTION_END
}

void do_statfs(fuse_req_t req, fuse_ino_t ino) {
	HANDLE_POLONAISE_EXCEPTION_BEGIN

	StatFsReply reply;
	gPolonaiseClient->statfs(reply, get_context(req), ino);
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


int main(int argc, char** argv) {
	gHostname = (getenv("POLONAISE_SERVER") ? getenv("POLONAISE_SERVER") : "localhost");
	gPort = (getenv("POLONAISE_PORT") ? atoi(getenv("POLONAISE_PORT")) : 9090);
	if (!polonaise_init()) {
		fprintf(stderr, "Server connection failed.\n");
		return 1;
	}
	struct fuse_lowlevel_ops ops{};
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
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	char *mp;
	if (fuse_parse_cmdline(&args, &mp, NULL, NULL) < 0) {
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
	fuse_session_loop(session);
	fuse_remove_signal_handlers(session);
	return 0;
}
