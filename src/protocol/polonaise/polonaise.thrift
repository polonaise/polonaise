namespace * polonaise

typedef i64 Inode
typedef i64 Timestamp
typedef i64 Descriptor
typedef i64 SessionId

const Descriptor kNullDescriptor = -1

enum StatusCode {
	kE2BIG = 1,
	kEACCES = 2,
	kEAGAIN = 3,
	kEBADF = 4,
	kEBUSY = 5,
	kECHILD = 6,
	kEDOM = 7,
	kEEXIST = 8,
	kEFAULT = 9,
	kEFBIG = 10,
	kEINTR = 11,
	kEINVAL = 12,
	kEIO = 13,
	kEISDIR = 14,
	kEMFILE = 15,
	kEMLINK = 16,
	kENAMETOOLONG = 17,
	kENFILE = 18,
	kENODATA = 19,
	kENODEV = 20,
	kENOENT = 21,
	kENOEXEC = 22,
	kENOMEM = 23,
	kENOSPC = 24,
	kENOSYS = 25,
	kENOTBLK = 26,
	kENOTDIR = 27,
	kENOTEMPTY = 28,
	kENOTTY = 29,
	kENXIO = 30,
	kEPERM = 31,
	kEPIPE = 32,
	kERANGE = 33,
	kEROFS = 34,
	kESPIPE = 35,
	kESRCH = 36,
	kETIMEDOUT = 37,
	kETXTBSY = 38,
	kEXDEV = 39,
}

enum FileType {
	kRegular = 1,
	kDirectory = 2,
	kSymlink = 3,
	kFifo = 4,
	kBlockDevice = 5,
	kCharDevice = 6,
	kSocket = 7,
}

enum OpenFlags {
	kRead = 1,
	kWrite = 2,
	kCreate = 4,
	kExclusive = 8,
	kTrunc = 16,
	kAppend = 32,
}

exception Failure {
	1: required string message
}

exception Status {
	1: required StatusCode statusCode
}

struct Context {
	1: required SessionId  sessionId
	2: required i64        uid
	3: required i64        gid
	4: required i64        pid
	5: required i64        umask
}

struct FileStat {
	5:  required FileType  type
	10: required i64       dev
	15: required Inode     inode
	20: required i64       nlink
	25: required i64       mode
	30: required i64       uid
	35: required i64       gid
	40: required i64       rdev
	45: required i64       size
	47: required i64       blockSize
	50: required i64       blocks
	55: required Timestamp atime
	60: required Timestamp mtime
	65: required Timestamp ctime
}

struct OpenReply {
	1: required Descriptor descriptor
	2: required bool       directIo
	3: required bool       keepCache
	4: required bool       nonSeekable
}


struct EntryReply {
	1: required Inode      inode
	2: required i64        generation
	3: required FileStat   attributes
	4: required double     attributesTimeout
	5: required double     entryTimeout
}

struct AttributesReply {
	1: required FileStat   attributes
	2: required double     attributesTimeout
}

struct ReadDirReply {
	1: required string     name
	2: required FileStat   attributes
	3: required i64        offset
	4: required i64        size
}

service Polonaise
{
	SessionId initSession() throws (1: Failure failure)

	EntryReply lookup(
			1: Context context,
			2: Inode inode,
			3: string name)
			throws (1: Status status, 2: Failure failure)

	AttributesReply getattr(
			1: Context context,
			2: Inode inode,
			3: Descriptor descriptor)
			throws (1: Status status, 2: Failure failure)

	OpenReply opendir(
			1: Context context,
			2: Inode inode,
			3: i32 flags)
			throws (1: Status status, 2: Failure failure)

	list<ReadDirReply> readdir(
			1: Context context,
			2: Inode inode,
			3: i64 offset,
			4: i64 size,
			5: Descriptor descriptor)
			throws (1: Status status, 2: Failure failure)

	void releasedir(
			1: Context context,
			2: Inode inode,
			3: Descriptor descriptor)
			throws (1: Status status, 2: Failure failure)
}
