#!/usr/bin/env python3
"""
XBE (Xbox Executable) Parser for Burnout 3 Static Recompilation Project.

Parses Xbox XBE files and extracts header information, section tables,
kernel imports, library versions, and certificate data.

Usage:
    python xbe_parser.py <path_to_xbe> [--extract-sections <output_dir>]
"""

import struct
import sys
import os
import argparse
from datetime import datetime, timezone
from dataclasses import dataclass, field
from typing import List, Optional, Dict
from pathlib import Path


# ============================================================
# Constants
# ============================================================

XBE_MAGIC = b'XBEH'

# Entry point XOR keys
ENTRY_RETAIL_XOR  = 0xA8FC57AB
ENTRY_DEBUG_XOR   = 0x94859D4B

# Kernel thunk XOR keys
THUNK_RETAIL_XOR  = 0x5B6D40B6
THUNK_DEBUG_XOR   = 0xEFB1F152

# Section flags
SECTION_WRITABLE         = 0x00000001
SECTION_PRELOAD          = 0x00000002
SECTION_EXECUTABLE       = 0x00000004
SECTION_INSERTED_FILE    = 0x00000008
SECTION_HEAD_PAGE_RO     = 0x00000010
SECTION_TAIL_PAGE_RO     = 0x00000020

# Init flags
INIT_MOUNT_UTILITY       = 0x00000001
INIT_FORMAT_UTILITY      = 0x00000002
INIT_LIMIT_64MB          = 0x00000004
INIT_DONT_SETUP_HARDDISK = 0x00000008

# Region flags
REGION_NA      = 0x00000001
REGION_JAPAN   = 0x00000002
REGION_REST    = 0x00000004
REGION_MANUFACTURING = 0x80000000

# Media type flags
MEDIA_HARD_DISK       = 0x00000001
MEDIA_DVD_X2          = 0x00000002
MEDIA_DVD_CD          = 0x00000004
MEDIA_CD              = 0x00000008
MEDIA_DVD_5_RO        = 0x00000010
MEDIA_DVD_9_RO        = 0x00000020
MEDIA_DVD_5_RW        = 0x00000040
MEDIA_DVD_9_RW        = 0x00000080
MEDIA_DONGLE          = 0x00000100
MEDIA_MEDIA_BOARD     = 0x00000200
MEDIA_NONSECURE_HD    = 0x40000000
MEDIA_NONSECURE_MODE  = 0x80000000

# Xbox kernel export names (ordinal → name)
# Based on https://xboxdevwiki.net/Kernel/Kernel_Exports
KERNEL_EXPORTS = {
    1: "AvGetSavedDataAddress",
    2: "AvSendTVEncoderOption",
    3: "AvSetDisplayMode",
    4: "AvSetSavedDataAddress",
    14: "DbgPrint",
    15: "ExAllocatePool",
    16: "ExAllocatePoolWithTag",
    17: "ExEventObjectType",
    18: "ExFreePool",
    22: "ExMutantObjectType",
    24: "ExQueryPoolBlockSize",
    25: "ExQueryNonVolatileSetting",
    26: "ExReadWriteRefurbInfo",
    27: "ExRaiseException",
    28: "ExRaiseStatus",
    29: "ExSaveNonVolatileSetting",
    30: "ExSemaphoreObjectType",
    35: "ExRegisterThreadNotification",
    36: "ExSetTimerResolution",
    37: "FscGetCacheSize",
    38: "FscInvalidateIdleBlocks",
    39: "FscSetCacheSize",
    40: "HalClearSoftwareInterrupt",
    41: "HalDisableSystemInterrupt",
    43: "HalEnableSystemInterrupt",
    44: "HalGetInterruptVector",
    46: "HalReadSMCTrayState",
    47: "HalReadWritePCISpace",
    48: "HalRegisterShutdownNotification",
    49: "HalRequestSoftwareInterrupt",
    50: "HalReturnToFirmware",
    51: "HalWriteSMBusValue",
    52: "InterlockedCompareExchange",
    53: "InterlockedDecrement",
    54: "InterlockedIncrement",
    55: "InterlockedExchange",
    56: "InterlockedExchangeAdd",
    57: "InterlockedFlushSList",
    58: "InterlockedPopEntrySList",
    59: "InterlockedPushEntrySList",
    60: "IoAllocateIrp",
    61: "IoBuildAsynchronousFsdRequest",
    62: "IoBuildDeviceIoControlRequest",
    63: "IoBuildSynchronousFsdRequest",
    64: "IoCheckShareAccess",
    65: "IoCompletionObjectType",
    66: "IoCreateDevice",
    67: "IoCreateFile",
    68: "IoCreateSymbolicLink",
    69: "IoDeleteDevice",
    70: "IoDeleteSymbolicLink",
    71: "IoDeviceObjectType",
    72: "IoFileObjectType",
    73: "IoFreeIrp",
    74: "IoInitializeIrp",
    76: "IoMarkIrpMustComplete",
    77: "IoQueryFileInformation",
    78: "IoQueryVolumeInformation",
    79: "IoQueueThreadIrp",
    80: "IoRemoveShareAccess",
    81: "IoSetIoCompletion",
    82: "IoSetShareAccess",
    83: "IoStartNextPacket",
    84: "IoStartNextPacketByKey",
    85: "IoStartPacket",
    86: "IoSynchronousDeviceIoControlRequest",
    87: "IoSynchronousFsdRequest",
    88: "IofCallDriver",
    89: "IofCompleteRequest",
    90: "KdDebuggerEnabled",
    91: "KdDebuggerNotPresent",
    92: "IoDismountVolume",
    93: "IoDismountVolumeByName",
    94: "KeAlertResumeThread",
    95: "KeAlertThread",
    96: "KeBoostPriorityThread",
    97: "KeBugCheck",
    98: "KeBugCheckEx",
    99: "KeCancelTimer",
    100: "KeConnectInterrupt",
    101: "KeDelayExecutionThread",
    102: "KeDisconnectInterrupt",
    103: "KeEnterCriticalRegion",
    104: "KeGetCurrentIrql",
    107: "KeInitializeDpc",
    108: "KeInitializeEvent",
    109: "KeInitializeInterrupt",
    110: "KeInitializeMutant",
    112: "KeInitializeSemaphore",
    113: "KeInitializeTimerEx",
    114: "KeInsertByKeyDeviceQueue",
    115: "KeInsertDeviceQueue",
    116: "KeInsertHeadQueue",
    117: "KeInsertQueue",
    118: "KeInsertQueueApc",
    119: "KeInsertQueueDpc",
    120: "KeInterruptTime",
    121: "KeIsExecutingDpc",
    122: "KeLeaveCriticalRegion",
    123: "KePulseEvent",
    124: "KeQueryBasePriorityThread",
    125: "KeQueryInterruptTime",
    126: "KeQueryPerformanceCounter",
    127: "KeQueryPerformanceFrequency",
    128: "KeQuerySystemTime",
    129: "KeRaiseIrqlToDpcLevel",
    130: "KeRaiseIrqlToSynchLevel",
    131: "KeReleaseMutant",
    132: "KeReleaseSemaphore",
    133: "KeRemoveByKeyDeviceQueue",
    134: "KeRemoveDeviceQueue",
    135: "KeRemoveEntryDeviceQueue",
    136: "KeRemoveQueue",
    137: "KeRemoveQueueDpc",
    138: "KeResetEvent",
    139: "KeRestoreFloatingPointState",
    140: "KeResumeThread",
    141: "KeRundownQueue",
    142: "KeSaveFloatingPointState",
    143: "KeSetBasePriorityThread",
    144: "KeSetDisableBoostThread",
    145: "KeSetEvent",
    146: "KeSetEventBoostPriority",
    147: "KeSetPriorityProcess",
    148: "KeSetPriorityThread",
    149: "KeSetTimer",
    150: "KeSetTimerEx",
    151: "KeStallExecutionProcessor",
    152: "KeSuspendThread",
    153: "KeSynchronizeExecution",
    154: "KeSystemTime",
    155: "KeTestAlertThread",
    156: "KeTickCount",
    157: "KeTimeIncrement",
    158: "KeWaitForMultipleObjects",
    159: "KeWaitForSingleObject",
    160: "KfRaiseIrql",
    161: "KfLowerIrql",
    162: "KiBugCheckData",
    163: "KiUnlockDispatcherDatabase",
    164: "LaunchDataPage",
    165: "MmAllocateContiguousMemory",
    166: "MmAllocateContiguousMemoryEx",
    167: "MmAllocateSystemMemory",
    168: "MmClaimGpuInstanceMemory",
    169: "MmCreateKernelStack",
    170: "MmDeleteKernelStack",
    171: "MmFreeContiguousMemory",
    172: "MmFreeSystemMemory",
    173: "MmGetPhysicalAddress",
    174: "MmIsAddressValid",
    175: "MmLockUnlockBufferPages",
    176: "MmLockUnlockPhysicalPage",
    177: "MmMapIoSpace",
    178: "MmPersistContiguousMemory",
    179: "MmQueryAddressProtect",
    180: "MmQueryAllocationSize",
    181: "MmQueryStatistics",
    182: "MmSetAddressProtect",
    183: "MmUnmapIoSpace",
    184: "NtAllocateVirtualMemory",
    185: "NtCancelTimer",
    186: "NtClearEvent",
    187: "NtClose",
    188: "NtCreateDirectoryObject",
    189: "NtCreateEvent",
    190: "NtCreateFile",
    191: "NtCreateIoCompletion",
    192: "NtCreateMutant",
    193: "NtCreateSemaphore",
    194: "NtCreateTimer",
    195: "NtDeleteFile",
    196: "NtDeviceIoControlFile",
    197: "NtDuplicateObject",
    198: "NtFlushBuffersFile",
    199: "NtFreeVirtualMemory",
    200: "NtFsControlFile",
    201: "NtOpenDirectoryObject",
    202: "NtOpenFile",
    203: "NtOpenSymbolicLinkObject",
    204: "NtProtectVirtualMemory",
    205: "NtPulseEvent",
    206: "NtQueueApcThread",
    207: "NtQueryDirectoryFile",
    208: "NtQueryDirectoryObject",
    209: "NtQueryEvent",
    210: "NtQueryFullAttributesFile",
    211: "NtQueryInformationFile",
    212: "NtQueryIoCompletion",
    213: "NtQueryMutant",
    214: "NtQuerySemaphore",
    215: "NtQuerySymbolicLinkObject",
    216: "NtQueryTimer",
    217: "NtQueryVirtualMemory",
    218: "NtQueryVolumeInformationFile",
    219: "NtReadFile",
    220: "NtReadFileScatter",
    221: "NtReleaseMutant",
    222: "NtReleaseSemaphore",
    223: "NtRemoveIoCompletion",
    224: "NtResumeThread",
    225: "NtSetEvent",
    226: "NtSetInformationFile",
    227: "NtSetIoCompletion",
    228: "NtSetSystemTime",
    229: "NtSetTimerEx",
    230: "NtSignalAndWaitForSingleObject",
    231: "NtSuspendThread",
    232: "NtUserIoApcDispatcher",
    233: "NtWaitForMultipleObjectsEx",
    234: "NtWaitForSingleObject",
    235: "NtWaitForSingleObjectEx",
    236: "NtWriteFile",
    237: "NtWriteFileGather",
    238: "NtYieldExecution",
    239: "ObCreateObject",
    240: "ObDirectoryObjectType",
    241: "ObInsertObject",
    242: "ObMakeTemporaryObject",
    243: "ObOpenObjectByName",
    244: "ObOpenObjectByPointer",
    245: "ObpObjectHandleTable",
    246: "ObReferenceObjectByHandle",
    247: "ObReferenceObjectByName",
    248: "ObReferenceObjectByPointer",
    249: "ObSymbolicLinkObjectType",
    250: "ObfDereferenceObject",
    251: "ObfReferenceObject",
    252: "PhyGetLinkState",
    253: "PhyInitialize",
    254: "PsCreateSystemThread",
    255: "PsCreateSystemThreadEx",
    256: "PsQueryStatistics",
    257: "PsSetCreateThreadNotifyRoutine",
    258: "PsTerminateSystemThread",
    259: "PsThreadObjectType",
    260: "RtlAnsiStringToUnicodeString",
    261: "RtlAppendStringToString",
    262: "RtlAppendUnicodeStringToString",
    263: "RtlAppendUnicodeToString",
    264: "RtlAssert",
    265: "RtlCaptureContext",
    266: "RtlCaptureStackBackTrace",
    267: "RtlCharToInteger",
    268: "RtlCompareMemory",
    269: "RtlCompareMemoryUlong",
    270: "RtlCompareString",
    271: "RtlCompareUnicodeString",
    272: "RtlCopyString",
    273: "RtlCopyUnicodeString",
    274: "RtlCreateUnicodeString",
    275: "RtlDowncaseUnicodeChar",
    276: "RtlDowncaseUnicodeString",
    277: "RtlEnterCriticalSection",
    278: "RtlEnterCriticalSectionAndRegion",
    279: "RtlEqualString",
    280: "RtlEqualUnicodeString",
    281: "RtlExtendedIntegerMultiply",
    282: "RtlExtendedLargeIntegerDivide",
    283: "RtlExtendedMagicDivide",
    284: "RtlFillMemory",
    285: "RtlFillMemoryUlong",
    286: "RtlFreeAnsiString",
    287: "RtlFreeUnicodeString",
    288: "RtlGetCallersAddress",
    289: "RtlInitAnsiString",
    290: "RtlInitUnicodeString",
    291: "RtlInitializeCriticalSection",
    292: "RtlIntegerToChar",
    293: "RtlIntegerToUnicodeString",
    294: "RtlLeaveCriticalSection",
    295: "RtlLeaveCriticalSectionAndRegion",
    296: "RtlLowerChar",
    297: "RtlMapGenericMask",
    298: "RtlMoveMemory",
    299: "RtlMultiByteToUnicodeN",
    300: "RtlMultiByteToUnicodeSize",
    301: "RtlNtStatusToDosError",
    302: "RtlRaiseException",
    303: "RtlRaiseStatus",
    304: "RtlTimeFieldsToTime",
    305: "RtlTimeToTimeFields",
    306: "RtlTryEnterCriticalSection",
    307: "RtlUlongByteSwap",
    308: "RtlUnicodeStringToAnsiString",
    309: "RtlUnicodeStringToInteger",
    310: "RtlUnicodeToMultiByteN",
    311: "RtlUnicodeToMultiByteSize",
    312: "RtlUnwind",
    313: "RtlUpcaseUnicodeChar",
    314: "RtlUpcaseUnicodeString",
    315: "RtlUpcaseUnicodeToMultiByteN",
    316: "RtlUpperChar",
    317: "RtlUpperString",
    318: "RtlUshortByteSwap",
    319: "RtlWalkFrameChain",
    320: "RtlZeroMemory",
    321: "XboxEEPROMKey",
    322: "XboxHardwareInfo",
    323: "XboxHDKey",
    324: "XboxKrnlVersion",
    325: "XboxSignatureKey",
    326: "XboxLANKey",
    327: "XboxAlternateSignatureKeys",
    328: "XeImageFileName",
    329: "XeLoadSection",
    330: "XeUnloadSection",
    331: "READ_PORT_BUFFER_UCHAR",
    332: "READ_PORT_BUFFER_USHORT",
    333: "READ_PORT_BUFFER_ULONG",
    334: "WRITE_PORT_BUFFER_UCHAR",
    335: "WRITE_PORT_BUFFER_USHORT",
    336: "WRITE_PORT_BUFFER_ULONG",
    337: "XcSHAInit",
    338: "XcSHAUpdate",
    339: "XcSHAFinal",
    340: "XcRC4Key",
    341: "XcRC4Crypt",
    342: "XcHMAC",
    343: "XcPKEncPublic",
    344: "XcPKDecPrivate",
    345: "XcPKGetKeyLen",
    346: "XcVerifyPKCS1Signature",
    347: "XcModExp",
    348: "XcDESKeyParity",
    349: "XcKeyTable",
    350: "XcBlockCrypt",
    351: "XcBlockCryptCBC",
    352: "XcCryptService",
    353: "XcUpdateCrypto",
    354: "RtlRip",
    355: "XboxLANKey",
    356: "XboxAlternateSignatureKeys",
    357: "XePublicKeyData",
    358: "HalIsResetOrShutdownPending",
    359: "IoMarkIrpMustComplete",
    360: "HalInitiateShutdown",
    361: "RtlSnprintf",
    362: "RtlSprintf",
    363: "RtlVsnprintf",
    364: "RtlVsprintf",
    365: "HalEnableSecureTrayEject",
    366: "HalWriteSMCScratchRegister",
}


# ============================================================
# Data Classes
# ============================================================

@dataclass
class XBEHeader:
    magic: bytes
    signature: bytes
    base_address: int          # 0x0104
    headers_size: int          # 0x0108
    image_size: int            # 0x010C
    image_header_size: int     # 0x0110
    timestamp: int             # 0x0114
    certificate_addr: int      # 0x0118
    num_sections: int          # 0x011C
    section_headers_addr: int  # 0x0120
    init_flags: int            # 0x0124
    entry_point_raw: int       # 0x0128 (encoded)
    tls_addr: int              # 0x012C
    pe_stack_commit: int       # 0x0130
    pe_heap_reserve: int       # 0x0134
    pe_heap_commit: int        # 0x0138
    pe_base_address: int       # 0x013C (original PE base)
    pe_size_of_image: int      # 0x0140 (original PE image size)
    pe_checksum: int           # 0x0144
    pe_timedate: int           # 0x0148
    debug_pathname_addr: int   # 0x014C
    debug_filename_addr: int   # 0x0150
    debug_unicode_filename_addr: int  # 0x0154
    kernel_thunk_addr_raw: int # 0x0158 (encoded)
    non_kernel_import_dir_addr: int   # 0x015C
    num_library_versions: int  # 0x0160
    library_versions_addr: int # 0x0164
    kernel_library_version_addr: int  # 0x0168
    xapi_library_version_addr: int    # 0x016C
    logo_bitmap_addr: int      # 0x0170
    logo_bitmap_size: int      # 0x0174

    # Decoded values
    entry_point: int = 0
    is_debug: bool = False
    kernel_thunk_addr: int = 0


@dataclass
class XBECertificate:
    size: int
    timestamp: int
    title_id: int
    title_name: str
    alt_title_ids: List[int]
    allowed_media: int
    game_region: int
    game_ratings: int
    disc_number: int
    version: int
    lan_key: bytes
    signature_key: bytes
    alt_signature_keys: List[bytes]


@dataclass
class XBESectionHeader:
    flags: int
    virtual_addr: int
    virtual_size: int
    raw_addr: int
    raw_size: int
    section_name_addr: int
    section_name_refcount: int
    head_shared_page_refcount_addr: int
    tail_shared_page_refcount_addr: int
    section_digest: bytes
    name: str = ""


@dataclass
class XBELibraryVersion:
    name: str
    major: int
    minor: int
    build: int
    flags: int


@dataclass
class XBEKernelImport:
    ordinal: int
    name: str
    thunk_addr: int


@dataclass
class XBETLSDirectory:
    raw_data_start: int
    raw_data_end: int
    tls_index_addr: int
    callbacks_addr: int
    zero_fill_size: int
    characteristics: int


@dataclass
class XBEFile:
    header: XBEHeader
    certificate: XBECertificate
    sections: List[XBESectionHeader]
    libraries: List[XBELibraryVersion]
    kernel_imports: List[XBEKernelImport]
    tls: Optional[XBETLSDirectory]
    debug_pathname: str
    debug_filename: str
    raw_data: bytes


# ============================================================
# Parser
# ============================================================

class XBEParser:
    def __init__(self, filepath: str):
        self.filepath = filepath
        with open(filepath, 'rb') as f:
            self.data = f.read()

    def _read_u32(self, offset: int) -> int:
        return struct.unpack_from('<I', self.data, offset)[0]

    def _read_u16(self, offset: int) -> int:
        return struct.unpack_from('<H', self.data, offset)[0]

    def _read_bytes(self, offset: int, size: int) -> bytes:
        return self.data[offset:offset + size]

    def _read_string(self, offset: int, max_len: int = 256) -> str:
        """Read null-terminated ASCII string."""
        end = self.data.find(b'\x00', offset, offset + max_len)
        if end == -1:
            end = offset + max_len
        return self.data[offset:end].decode('ascii', errors='replace')

    def _read_wstring(self, offset: int, max_chars: int = 128) -> str:
        """Read null-terminated UTF-16LE string."""
        chars = []
        for i in range(max_chars):
            c = struct.unpack_from('<H', self.data, offset + i * 2)[0]
            if c == 0:
                break
            chars.append(chr(c))
        return ''.join(chars)

    def _va_to_file(self, va: int, base: int) -> int:
        """Convert virtual address to file offset using section headers."""
        # For addresses within the header
        if va < base + 0x1000:
            return va - base
        # Search sections
        for section in self._sections:
            sec_va_start = section.virtual_addr
            sec_va_end = sec_va_start + section.virtual_size
            if sec_va_start <= va < sec_va_end:
                return section.raw_addr + (va - sec_va_start)
        # Fallback: assume flat mapping
        return va - base

    def parse(self) -> XBEFile:
        # Validate magic
        magic = self._read_bytes(0, 4)
        if magic != XBE_MAGIC:
            raise ValueError(f"Invalid XBE magic: {magic!r} (expected {XBE_MAGIC!r})")

        # Parse header
        header = self._parse_header()

        # Parse sections first (needed for VA→file translation)
        self._sections = self._parse_sections(header)

        # Parse certificate
        cert = self._parse_certificate(header)

        # Parse library versions
        libraries = self._parse_libraries(header)

        # Parse kernel imports
        kernel_imports = self._parse_kernel_imports(header)

        # Parse TLS
        tls = self._parse_tls(header)

        # Read debug strings
        debug_pathname = ""
        debug_filename = ""
        if header.debug_pathname_addr:
            try:
                off = self._va_to_file(header.debug_pathname_addr, header.base_address)
                debug_pathname = self._read_string(off)
            except Exception:
                pass
        if header.debug_filename_addr:
            try:
                off = self._va_to_file(header.debug_filename_addr, header.base_address)
                debug_filename = self._read_string(off)
            except Exception:
                pass

        return XBEFile(
            header=header,
            certificate=cert,
            sections=self._sections,
            libraries=libraries,
            kernel_imports=kernel_imports,
            tls=tls,
            debug_pathname=debug_pathname,
            debug_filename=debug_filename,
            raw_data=self.data,
        )

    def _parse_header(self) -> XBEHeader:
        h = XBEHeader(
            magic=self._read_bytes(0, 4),
            signature=self._read_bytes(0x0004, 256),
            base_address=self._read_u32(0x0104),
            headers_size=self._read_u32(0x0108),
            image_size=self._read_u32(0x010C),
            image_header_size=self._read_u32(0x0110),
            timestamp=self._read_u32(0x0114),
            certificate_addr=self._read_u32(0x0118),
            num_sections=self._read_u32(0x011C),
            section_headers_addr=self._read_u32(0x0120),
            init_flags=self._read_u32(0x0124),
            entry_point_raw=self._read_u32(0x0128),
            tls_addr=self._read_u32(0x012C),
            pe_stack_commit=self._read_u32(0x0130),
            pe_heap_reserve=self._read_u32(0x0134),
            pe_heap_commit=self._read_u32(0x0138),
            pe_base_address=self._read_u32(0x013C),
            pe_size_of_image=self._read_u32(0x0140),
            pe_checksum=self._read_u32(0x0144),
            pe_timedate=self._read_u32(0x0148),
            debug_pathname_addr=self._read_u32(0x014C),
            debug_filename_addr=self._read_u32(0x0150),
            debug_unicode_filename_addr=self._read_u32(0x0154),
            kernel_thunk_addr_raw=self._read_u32(0x0158),
            non_kernel_import_dir_addr=self._read_u32(0x015C),
            num_library_versions=self._read_u32(0x0160),
            library_versions_addr=self._read_u32(0x0164),
            kernel_library_version_addr=self._read_u32(0x0168),
            xapi_library_version_addr=self._read_u32(0x016C),
            logo_bitmap_addr=self._read_u32(0x0170),
            logo_bitmap_size=self._read_u32(0x0174),
        )

        # Decode entry point
        entry_retail = (h.entry_point_raw ^ ENTRY_RETAIL_XOR) & 0xFFFFFFFF
        entry_debug = (h.entry_point_raw ^ ENTRY_DEBUG_XOR) & 0xFFFFFFFF

        # Determine if retail or debug based on which entry falls in valid range
        if h.base_address <= entry_retail < h.base_address + h.image_size:
            h.entry_point = entry_retail
            h.is_debug = False
        elif h.base_address <= entry_debug < h.base_address + h.image_size:
            h.entry_point = entry_debug
            h.is_debug = True
        else:
            # Default to retail
            h.entry_point = entry_retail
            h.is_debug = False

        # Decode kernel thunk address
        thunk_retail = (h.kernel_thunk_addr_raw ^ THUNK_RETAIL_XOR) & 0xFFFFFFFF
        thunk_debug = (h.kernel_thunk_addr_raw ^ THUNK_DEBUG_XOR) & 0xFFFFFFFF

        if h.is_debug:
            h.kernel_thunk_addr = thunk_debug
        else:
            h.kernel_thunk_addr = thunk_retail

        return h

    def _parse_sections(self, header: XBEHeader) -> List[XBESectionHeader]:
        sections = []
        base = header.base_address
        sec_hdr_offset = header.section_headers_addr - base

        for i in range(header.num_sections):
            off = sec_hdr_offset + i * 56  # Each section header is 56 bytes
            sec = XBESectionHeader(
                flags=self._read_u32(off + 0),
                virtual_addr=self._read_u32(off + 4),
                virtual_size=self._read_u32(off + 8),
                raw_addr=self._read_u32(off + 12),
                raw_size=self._read_u32(off + 16),
                section_name_addr=self._read_u32(off + 20),
                section_name_refcount=self._read_u32(off + 24),
                head_shared_page_refcount_addr=self._read_u32(off + 28),
                tail_shared_page_refcount_addr=self._read_u32(off + 32),
                section_digest=self._read_bytes(off + 36, 20),
            )
            # Read section name
            if sec.section_name_addr:
                name_off = sec.section_name_addr - base
                if 0 <= name_off < len(self.data):
                    sec.name = self._read_string(name_off, 32)
            sections.append(sec)

        return sections

    def _parse_certificate(self, header: XBEHeader) -> XBECertificate:
        base = header.base_address
        off = header.certificate_addr - base

        title_name_raw = self._read_bytes(off + 12, 80)
        title_name = title_name_raw.decode('utf-16-le', errors='replace').rstrip('\x00')

        alt_title_ids = []
        for i in range(16):
            alt_title_ids.append(self._read_u32(off + 92 + i * 4))

        alt_sig_keys = []
        for i in range(16):
            alt_sig_keys.append(self._read_bytes(off + 0x01AC + i * 16, 16))

        return XBECertificate(
            size=self._read_u32(off + 0),
            timestamp=self._read_u32(off + 4),
            title_id=self._read_u32(off + 8),
            title_name=title_name,
            alt_title_ids=alt_title_ids,
            allowed_media=self._read_u32(off + 156),
            game_region=self._read_u32(off + 160),
            game_ratings=self._read_u32(off + 164),
            disc_number=self._read_u32(off + 168),
            version=self._read_u32(off + 172),
            lan_key=self._read_bytes(off + 176, 16),
            signature_key=self._read_bytes(off + 192, 16),
            alt_signature_keys=alt_sig_keys,
        )

    def _parse_libraries(self, header: XBEHeader) -> List[XBELibraryVersion]:
        if not header.library_versions_addr or header.num_library_versions == 0:
            return []

        libraries = []
        base = header.base_address
        off = header.library_versions_addr - base

        for i in range(header.num_library_versions):
            lib_off = off + i * 16  # Each library version entry is 16 bytes
            name_bytes = self._read_bytes(lib_off, 8)
            name = name_bytes.decode('ascii', errors='replace').rstrip('\x00')
            major = self._read_u16(lib_off + 8)
            minor = self._read_u16(lib_off + 10)
            build = self._read_u16(lib_off + 12)
            flags = self._read_u16(lib_off + 14)
            libraries.append(XBELibraryVersion(
                name=name, major=major, minor=minor, build=build, flags=flags
            ))

        return libraries

    def _parse_kernel_imports(self, header: XBEHeader) -> List[XBEKernelImport]:
        if not header.kernel_thunk_addr:
            return []

        imports = []
        base = header.base_address

        # Find the section containing the thunk table
        thunk_file_off = None
        for sec in self._sections:
            sec_va_start = sec.virtual_addr
            sec_va_end = sec_va_start + sec.virtual_size
            if sec_va_start <= header.kernel_thunk_addr < sec_va_end:
                thunk_file_off = sec.raw_addr + (header.kernel_thunk_addr - sec_va_start)
                break

        if thunk_file_off is None:
            # Try header area
            thunk_file_off = header.kernel_thunk_addr - base

        off = thunk_file_off
        while off + 4 <= len(self.data):
            val = self._read_u32(off)
            if val == 0:
                break

            if val & 0x80000000:
                ordinal = val & 0x7FFFFFFF
                name = KERNEL_EXPORTS.get(ordinal, f"Unknown_{ordinal}")
                imports.append(XBEKernelImport(
                    ordinal=ordinal,
                    name=name,
                    thunk_addr=header.kernel_thunk_addr + (off - thunk_file_off),
                ))
            off += 4

        return imports

    def _parse_tls(self, header: XBEHeader) -> Optional[XBETLSDirectory]:
        if not header.tls_addr:
            return None

        base = header.base_address
        off = self._va_to_file(header.tls_addr, base)

        return XBETLSDirectory(
            raw_data_start=self._read_u32(off + 0),
            raw_data_end=self._read_u32(off + 4),
            tls_index_addr=self._read_u32(off + 8),
            callbacks_addr=self._read_u32(off + 12),
            zero_fill_size=self._read_u32(off + 16),
            characteristics=self._read_u32(off + 20),
        )

    def extract_section(self, section: XBESectionHeader, output_dir: str) -> str:
        """Extract a section's raw data to a file."""
        os.makedirs(output_dir, exist_ok=True)
        safe_name = section.name.replace('$', '_').replace('.', '_').strip('_')
        if not safe_name:
            safe_name = f"section_{section.virtual_addr:08X}"
        filename = f"{safe_name}.bin"
        filepath = os.path.join(output_dir, filename)

        raw = self.data[section.raw_addr:section.raw_addr + section.raw_size]
        with open(filepath, 'wb') as f:
            f.write(raw)

        return filepath


# ============================================================
# Pretty Printer
# ============================================================

def format_flags(flags: int) -> str:
    parts = []
    if flags & SECTION_WRITABLE:
        parts.append("W")
    if flags & SECTION_PRELOAD:
        parts.append("PRE")
    if flags & SECTION_EXECUTABLE:
        parts.append("X")
    if flags & SECTION_INSERTED_FILE:
        parts.append("INS")
    if flags & SECTION_HEAD_PAGE_RO:
        parts.append("HRO")
    if flags & SECTION_TAIL_PAGE_RO:
        parts.append("TRO")
    return ", ".join(parts) if parts else "NONE"


def format_region(region: int) -> str:
    parts = []
    if region & REGION_NA:
        parts.append("North America")
    if region & REGION_JAPAN:
        parts.append("Japan")
    if region & REGION_REST:
        parts.append("Rest of World")
    if region & REGION_MANUFACTURING:
        parts.append("Manufacturing")
    return ", ".join(parts) if parts else "Unknown"


def format_media(media: int) -> str:
    parts = []
    if media & MEDIA_HARD_DISK:
        parts.append("HDD")
    if media & MEDIA_DVD_X2:
        parts.append("DVD_X2")
    if media & MEDIA_DVD_CD:
        parts.append("DVD_CD")
    if media & MEDIA_CD:
        parts.append("CD")
    if media & MEDIA_DVD_5_RO:
        parts.append("DVD5_RO")
    if media & MEDIA_DVD_9_RO:
        parts.append("DVD9_RO")
    return ", ".join(parts) if parts else "Unknown"


def format_timestamp(ts: int) -> str:
    try:
        dt = datetime.fromtimestamp(ts, tz=timezone.utc)
        return dt.strftime('%Y-%m-%d %H:%M:%S UTC')
    except (OSError, ValueError):
        return f"0x{ts:08X}"


def print_xbe_info(xbe: XBEFile):
    h = xbe.header
    c = xbe.certificate

    print("=" * 70)
    print("  XBE Analysis Report")
    print("=" * 70)

    print(f"\n  File: {c.title_name}")
    print(f"  Title ID: 0x{c.title_id:08X}")
    print(f"  Type: {'Debug' if h.is_debug else 'Retail'}")
    print(f"  Build Date: {format_timestamp(h.timestamp)}")
    print(f"  Certificate Date: {format_timestamp(c.timestamp)}")
    print(f"  Region: {format_region(c.game_region)}")
    print(f"  Media: {format_media(c.allowed_media)}")
    print(f"  Disc: {c.disc_number}, Version: {c.version}")
    print(f"  Debug Path: {xbe.debug_pathname}")

    print(f"\n--- Memory Layout ---")
    print(f"  Base Address: 0x{h.base_address:08X}")
    print(f"  Image Size: 0x{h.image_size:08X} ({h.image_size / 1024 / 1024:.2f} MB)")
    print(f"  Entry Point: 0x{h.entry_point:08X} (raw: 0x{h.entry_point_raw:08X})")
    print(f"  Kernel Thunks: 0x{h.kernel_thunk_addr:08X} (raw: 0x{h.kernel_thunk_addr_raw:08X})")
    print(f"  Stack Commit: 0x{h.pe_stack_commit:X}")
    print(f"  Heap: reserve=0x{h.pe_heap_reserve:X} commit=0x{h.pe_heap_commit:X}")

    if xbe.tls:
        t = xbe.tls
        print(f"\n--- TLS ---")
        print(f"  Data: 0x{t.raw_data_start:08X} - 0x{t.raw_data_end:08X}")
        print(f"  Index Addr: 0x{t.tls_index_addr:08X}")
        print(f"  Zero Fill: {t.zero_fill_size} bytes")

    print(f"\n--- Sections ({h.num_sections}) ---")
    print(f"  {'#':>2}  {'Name':<12} {'VirtAddr':>10} {'VirtSize':>10} {'RawAddr':>10} {'RawSize':>10}  Flags")
    print(f"  {'':->2}  {'':->12} {'':->10} {'':->10} {'':->10} {'':->10}  {'':->20}")
    for i, sec in enumerate(xbe.sections):
        print(f"  {i:2d}  {sec.name:<12} 0x{sec.virtual_addr:08X} 0x{sec.virtual_size:08X} "
              f"0x{sec.raw_addr:08X} 0x{sec.raw_size:08X}  {format_flags(sec.flags)}")

    print(f"\n--- Libraries ({len(xbe.libraries)}) ---")
    for lib in xbe.libraries:
        print(f"  {lib.name:<12} {lib.major}.{lib.minor}.{lib.build}  flags=0x{lib.flags:04X}")

    print(f"\n--- Kernel Imports ({len(xbe.kernel_imports)}) ---")
    # Group by prefix
    groups: Dict[str, List[XBEKernelImport]] = {}
    for imp in xbe.kernel_imports:
        # Extract prefix (e.g., "Nt", "Ke", "Mm", "Rtl")
        prefix = ""
        for j, ch in enumerate(imp.name):
            if ch.isupper() and j > 0:
                break
            prefix += ch
        # Normalize some prefixes
        if imp.name.startswith("Nt"):
            prefix = "Nt"
        elif imp.name.startswith("Ke"):
            prefix = "Ke"
        elif imp.name.startswith("Kf"):
            prefix = "Kf"
        elif imp.name.startswith("Mm"):
            prefix = "Mm"
        elif imp.name.startswith("Rtl"):
            prefix = "Rtl"
        elif imp.name.startswith("Ps"):
            prefix = "Ps"
        elif imp.name.startswith("Ex"):
            prefix = "Ex"
        elif imp.name.startswith("Ob"):
            prefix = "Ob"
        elif imp.name.startswith("Io"):
            prefix = "Io"
        elif imp.name.startswith("Hal"):
            prefix = "Hal"
        elif imp.name.startswith("Av"):
            prefix = "Av"
        elif imp.name.startswith("Xc"):
            prefix = "Xc"
        elif imp.name.startswith("Xe") or imp.name.startswith("Xbox"):
            prefix = "Xbox"
        else:
            prefix = "Other"

        groups.setdefault(prefix, []).append(imp)

    for prefix in sorted(groups.keys()):
        imps = groups[prefix]
        print(f"\n  [{prefix}] ({len(imps)} imports)")
        for imp in sorted(imps, key=lambda x: x.ordinal):
            print(f"    #{imp.ordinal:<4d} {imp.name}")

    print(f"\n--- Certificate ---")
    print(f"  LAN Key: {c.lan_key.hex()}")
    print(f"  Signature Key: {c.signature_key.hex()}")
    print(f"  Game Ratings: 0x{c.game_ratings:08X}")

    print("\n" + "=" * 70)


# ============================================================
# JSON Export
# ============================================================

def export_json(xbe: XBEFile, output_path: str):
    """Export XBE analysis to JSON for use by other tools."""
    import json

    data = {
        "title": xbe.certificate.title_name,
        "title_id": f"0x{xbe.certificate.title_id:08X}",
        "type": "debug" if xbe.header.is_debug else "retail",
        "build_date": format_timestamp(xbe.header.timestamp),
        "xdk_version": xbe.libraries[0].build if xbe.libraries else 0,
        "build_path": xbe.debug_pathname,
        "base_address": f"0x{xbe.header.base_address:08X}",
        "image_size": xbe.header.image_size,
        "entry_point": f"0x{xbe.header.entry_point:08X}",
        "kernel_thunk_addr": f"0x{xbe.header.kernel_thunk_addr:08X}",
        "sections": [
            {
                "name": sec.name,
                "virtual_addr": f"0x{sec.virtual_addr:08X}",
                "virtual_size": sec.virtual_size,
                "raw_addr": f"0x{sec.raw_addr:08X}",
                "raw_size": sec.raw_size,
                "flags": format_flags(sec.flags),
                "writable": bool(sec.flags & SECTION_WRITABLE),
                "executable": bool(sec.flags & SECTION_EXECUTABLE),
            }
            for sec in xbe.sections
        ],
        "libraries": [
            {
                "name": lib.name,
                "version": f"{lib.major}.{lib.minor}.{lib.build}",
                "flags": f"0x{lib.flags:04X}",
            }
            for lib in xbe.libraries
        ],
        "kernel_imports": [
            {
                "ordinal": imp.ordinal,
                "name": imp.name,
                "thunk_addr": f"0x{imp.thunk_addr:08X}",
            }
            for imp in xbe.kernel_imports
        ],
        "tls": {
            "index_addr": f"0x{xbe.tls.tls_index_addr:08X}",
            "zero_fill_size": xbe.tls.zero_fill_size,
        } if xbe.tls else None,
    }

    with open(output_path, 'w') as f:
        json.dump(data, f, indent=2)

    print(f"JSON exported to: {output_path}")


# ============================================================
# Main
# ============================================================

def main():
    parser = argparse.ArgumentParser(
        description="XBE (Xbox Executable) Parser for Burnout 3 Static Recompilation"
    )
    parser.add_argument("xbe_path", help="Path to the XBE file")
    parser.add_argument("--extract-sections", metavar="DIR",
                        help="Extract all sections to the specified directory")
    parser.add_argument("--json", metavar="FILE",
                        help="Export analysis to JSON file")
    parser.add_argument("--quiet", action="store_true",
                        help="Suppress console output")

    args = parser.parse_args()

    if not os.path.exists(args.xbe_path):
        print(f"Error: File not found: {args.xbe_path}", file=sys.stderr)
        sys.exit(1)

    xbe_parser = XBEParser(args.xbe_path)
    xbe = xbe_parser.parse()

    if not args.quiet:
        print_xbe_info(xbe)

    if args.json:
        export_json(xbe, args.json)

    if args.extract_sections:
        print(f"\nExtracting sections to: {args.extract_sections}")
        for sec in xbe.sections:
            filepath = xbe_parser.extract_section(sec, args.extract_sections)
            print(f"  {sec.name:<12} → {filepath} ({sec.raw_size} bytes)")


if __name__ == "__main__":
    main()
