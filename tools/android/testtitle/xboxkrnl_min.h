/*
 * The handful of Xbox kernel declarations xbrtest.c needs. Signatures follow
 * the Xbox kernel (not NT): ANSI object names, nine-argument NtCreateFile,
 * PsCreateSystemThreadEx's ten arguments. See xboxkrnl.def for ordinals.
 */
#ifndef XBOXKRNL_MIN_H
#define XBOXKRNL_MIN_H

typedef unsigned char      UCHAR, BOOLEAN;
typedef unsigned short     USHORT;
typedef unsigned long      ULONG, ACCESS_MASK;
typedef long               NTSTATUS, LONG;
typedef unsigned long long ULONGLONG;
typedef long long          LONGLONG;
typedef void              *PVOID, *HANDLE;
typedef char               CHAR, *PCHAR;
typedef ULONG              ULONG_PTR;

typedef union { struct { ULONG LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER;

typedef struct { USHORT Length; USHORT MaximumLength; PCHAR Buffer; } ANSI_STRING;

typedef struct { HANDLE RootDirectory; ANSI_STRING *ObjectName; ULONG Attributes; } OBJECT_ATTRIBUTES;

typedef struct { union { NTSTATUS Status; PVOID Pointer; } u; ULONG_PTR Information; } IO_STATUS_BLOCK;

typedef void (__stdcall *PKSYSTEM_ROUTINE)(PVOID StartContext1, PVOID StartContext2);

#define NTAPI __stdcall
#define KIMPORT __declspec(dllimport)

#define NT_SUCCESS(s)               ((NTSTATUS)(s) >= 0)
#define GENERIC_READ                0x80000000u
#define GENERIC_WRITE               0x40000000u
#define SYNCHRONIZE                 0x00100000u
#define FILE_READ_ATTRIBUTES        0x00000080u
#define FILE_ATTRIBUTE_NORMAL       0x00000080u
#define FILE_SHARE_READ             0x00000001u
#define FILE_OPEN                   1u
#define FILE_OVERWRITE_IF           5u
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020u
#define FILE_NON_DIRECTORY_FILE     0x00000040u
#define OBJ_CASE_INSENSITIVE        0x00000040u

KIMPORT void      NTAPI AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode,
                                         ULONG Format, ULONG Pitch, ULONG FrameBuffer);
KIMPORT ULONG   __cdecl DbgPrint(const char *Format, ...);
KIMPORT NTSTATUS  NTAPI KeDelayExecutionThread(CHAR WaitMode, BOOLEAN Alertable,
                                               LARGE_INTEGER *Interval);
KIMPORT ULONGLONG NTAPI KeQueryPerformanceCounter(void);
KIMPORT ULONGLONG NTAPI KeQueryPerformanceFrequency(void);
KIMPORT void      NTAPI KeQuerySystemTime(LARGE_INTEGER *CurrentTime);
KIMPORT PVOID     NTAPI MmAllocateContiguousMemory(ULONG NumberOfBytes);
KIMPORT ULONG_PTR NTAPI MmGetPhysicalAddress(PVOID BaseAddress);
KIMPORT NTSTATUS  NTAPI NtClose(HANDLE Handle);
KIMPORT NTSTATUS  NTAPI NtCreateFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess,
                                     OBJECT_ATTRIBUTES *ObjectAttributes,
                                     IO_STATUS_BLOCK *IoStatusBlock,
                                     LARGE_INTEGER *AllocationSize, ULONG FileAttributes,
                                     ULONG ShareAccess, ULONG CreateDisposition,
                                     ULONG CreateOptions);
KIMPORT NTSTATUS  NTAPI NtReadFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine,
                                   PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock,
                                   PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset);
KIMPORT NTSTATUS  NTAPI NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine,
                                    PVOID ApcContext, IO_STATUS_BLOCK *IoStatusBlock,
                                    PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset);
KIMPORT NTSTATUS  NTAPI PsCreateSystemThreadEx(HANDLE *ThreadHandle, ULONG ThreadExtensionSize,
                                               ULONG KernelStackSize, ULONG TlsDataSize,
                                               ULONG *ThreadId, PVOID StartContext1,
                                               PVOID StartContext2, BOOLEAN CreateSuspended,
                                               BOOLEAN DebugStack, PKSYSTEM_ROUTINE StartRoutine);
KIMPORT void      NTAPI PsTerminateSystemThread(NTSTATUS ExitStatus);

#endif
