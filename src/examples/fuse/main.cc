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

static boost::shared_ptr<apache::thrift::transport::TTransport> gThriftTransport;
static boost::shared_ptr<PolonaiseClient> gPolonaiseClient;
static SessionId gSessionId;

bool polonaise_init(const char* hostname) {
	using namespace apache::thrift;
	using namespace apache::thrift::protocol;
	using namespace apache::thrift::transport;
	if (hostname == nullptr) {
		hostname = "localhost";
	}

	try {
		boost::shared_ptr<TTransport> socket(new TSocket(hostname, 9090));
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
	stbuf.st_mode |= fs.mode & 07777;
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

#define HANDLE_POLONAISE_EXCEPTION_BEGIN try {
#define HANDLE_POLONAISE_EXCEPTION_END \
	} catch (Status& s) { \
		fuse_reply_err(req, s.statusCode); \
	} catch (Failure& f) { \
		fprintf(stderr, "failure: %s\n", f.message.c_str()); \
		fuse_reply_err(req, EIO); \
	} catch (apache::thrift::TException& e) { \
		fprintf(stderr, "connection error: %s\n", e.what()); \
		fuse_reply_err(req, EIO); \
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

int main(int argc, char** argv) {
	if (!polonaise_init(getenv("POLONAISE_SERVER"))) {
		fprintf(stderr, "Server connection failed.\n");
		return 1;
	}
	struct fuse_lowlevel_ops ops{};
	ops.getattr = do_getattr;
	ops.opendir = do_opendir;
	ops.readdir = do_readdir;
	ops.releasedir = do_releasedir;
	ops.lookup = do_lookup;
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
