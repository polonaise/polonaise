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

enum AccessMask {
	kExecute = 1,
	kWrite = 2,
	kRead = 4
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

struct StatFsReply {
	1: required i64        filesystemId
	2: required i64        maxNameLength
	3: required i64        blockSize
	4: required i64        totalBlocks
	5: required i64        freeBlocks
	6: required i64        availableBlocks
	7: required i64        totalFiles
	8: required i64        freeFiles
	9: required i64        availableFiles
}

struct DirectoryEntry {
	1: required string     name
	2: required FileStat   attributes
	3: required i64        nextEntryOffset
}

service Polonaise
{
	SessionId initSession() throws (1: Failure failure)

	# Converts (<parent_inode>, <file_name>) into (<child_inode>, <child_attributes>).
	EntryReply lookup(
			1: Context context,
			2: Inode inode,
			3: string name)
			throws (1: Status status, 2: Failure failure)

	# Given inode, returns attributes of the file.
	# 'descriptor' is optional and should be provided if the file is opened by the callee,
	# otherise kNullDescriptor should be used.
	AttributesReply getattr(
			1: Context context,
			2: Inode inode,
			3: Descriptor descriptor)
			throws (1: Status status, 2: Failure failure)


	# Opens a directory.
	# Returns descriptor which should be used in forthcoming 'readdir' and 'releasedir' calls.
	Descriptor opendir(
			1: Context context,
			2: Inode inode)
			throws (1: Status status, 2: Failure failure)

	# Gets entries from a directory.
	# The directory has to be opened and descriptor returned by 'opendir' has to be provided.
	# 'maxNumberOfEntries' is the maximum number of entries returned by this call. If the call
	# returns less, this indicated that there are no more entries.
	# In the first call use 'firstEntryOffset' == 0. If readdir returns exactly
	# 'maxNumberOfEntries' entries, it indicated that there may be more entries present. To fetch
	# them, call readdir once again using 'firstEntryOffset' equal to 'entry.nextEntryOffset'
	# from the last entry returned by the previous call. Exact meaning of the 'firstEntryOffset'
	# and 'entry.nextEntryOffset' is implementation specific.
	list<DirectoryEntry> readdir(
			1: Context context,
			2: Inode inode,
			3: i64 firstEntryOffset,
			4: i64 maxNumberOfEntries,
			5: Descriptor descriptor)
			throws (1: Status status, 2: Failure failure)

	# Closes a directory.
	# The directory has to be opened and descriptor returned by 'opendir' has to be provided.
	void releasedir(
			1: Context context,
			2: Inode inode,
			3: Descriptor descriptor)
			throws (1: Status status, 2: Failure failure)

	# Checks access rights.
	# Throws if the access is not possible.
	# 'mask' is a bitwise alternative of values from the AccessMask enum.
	void access(
			1: Context context,
			2: Inode inode,
			3: i32 mask)
			throws (1: Status status, 2: Failure failure)

	# Opens a file.
	# 'flags' is a bitwise alternative of values from the OpenFlags enum.
	OpenReply open(
			1: Context context,
			2: Inode inode,
			3: i32 flags)
			throws (1: Status status, 2: Failure failure)

	# Gets data from an opened file.
	# The file has to be opened and descriptor returned by 'open' has to be provided.
	binary read(
			1: Context context,
			2: Inode inode,
			3: i64 offset,
			4: i64 size,
			5: Descriptor descriptor)
			throws (1: Status status, 2: Failure failure)

	# Flushes a file.
	# This has to be called each time when any copy of a descriptor
	# returned by 'open' is going to be closed.
	void flush(
			1: Context context,
			2: Inode inode,
			3: Descriptor descriptor)
			throws (1: Status status, 2: Failure failure)

	# Deletes a descriptor.
	# This should be called when the last copy of a descriptor returned by 'open' is closed.
	# Never throws the 'Status' exception.
	void release(
			1: Context context,
			2: Inode inode,
			3: Descriptor descriptor)
			throws (2: Failure failure)

	# Returns information about the filesystem.
	StatFsReply statfs(
			1: Context context,
			2: Inode inode)
			throws (1: Status status, 2: Failure failure)
}
