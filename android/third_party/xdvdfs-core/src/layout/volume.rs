use bincode::Options;
use serde::{Deserialize, Serialize};
use serde_big_array::BigArray;

use super::DirectoryEntryTable;

pub const VOLUME_HEADER_MAGIC: [u8; 0x14] = *b"MICROSOFT*XBOX*MEDIA";

/// XDVDFS volume information, located at sector 32 on the disk
#[repr(C)]
#[repr(packed)]
#[derive(Deserialize, Serialize, Debug, Copy, Clone, Eq, PartialEq)]
pub struct VolumeDescriptor {
    magic0: [u8; 0x14],
    pub root_table: DirectoryEntryTable,
    pub filetime: u64,

    #[serde(with = "BigArray")]
    unused: [u8; 0x7c8],

    magic1: [u8; 0x14],
}

impl VolumeDescriptor {
    pub fn new(root_table: DirectoryEntryTable) -> Self {
        Self {
            magic0: VOLUME_HEADER_MAGIC,
            root_table,
            filetime: 0,
            unused: [0; 0x7c8],
            magic1: VOLUME_HEADER_MAGIC,
        }
    }

    pub fn is_valid(&self) -> bool {
        self.magic0 == VOLUME_HEADER_MAGIC && self.magic1 == VOLUME_HEADER_MAGIC
    }

    pub fn serialize(&self) -> Result<alloc::vec::Vec<u8>, bincode::Error> {
        bincode::DefaultOptions::new()
            .with_fixint_encoding()
            .with_little_endian()
            .serialize(self)
    }

    pub fn deserialize(buf: &[u8; 0x800]) -> Result<Self, bincode::Error> {
        bincode::DefaultOptions::new()
            .with_fixint_encoding()
            .with_little_endian()
            .deserialize(buf)
    }
}

#[cfg(test)]
mod test {
    use crate::layout::{DirectoryEntryTable, DiskRegion, VOLUME_HEADER_MAGIC};

    use super::VolumeDescriptor;

    #[test]
    fn test_layout_volume_create() {
        let root_table = DirectoryEntryTable {
            region: DiskRegion { size: 0, sector: 0 },
        };

        let volume = VolumeDescriptor::new(root_table);
        assert!(volume.is_valid());
    }

    #[test]
    fn test_layout_volume_invalid_magic0() {
        let root_table = DirectoryEntryTable {
            region: DiskRegion { size: 0, sector: 0 },
        };

        let mut volume = VolumeDescriptor::new(root_table);
        volume.magic0[0] = 1;
        assert!(!volume.is_valid());
    }

    #[test]
    fn test_layout_volume_invalid_magic1() {
        let root_table = DirectoryEntryTable {
            region: DiskRegion { size: 0, sector: 0 },
        };

        let mut volume = VolumeDescriptor::new(root_table);
        volume.magic1[0] = 1;
        assert!(!volume.is_valid());
    }

    #[test]
    fn test_layout_volume_serialize() {
        let root_table = DirectoryEntryTable {
            region: DiskRegion {
                size: 10,
                sector: 20,
            },
        };

        let volume = VolumeDescriptor::new(root_table);
        assert!(volume.is_valid());

        let serialized = volume.serialize().expect("Serialization should succeed");
        assert_eq!(serialized[0..0x14], VOLUME_HEADER_MAGIC);
        assert_eq!(
            u32::from_le_bytes(serialized[0x14..0x18].try_into().unwrap()),
            20
        );
        assert_eq!(
            u32::from_le_bytes(serialized[0x18..0x1C].try_into().unwrap()),
            10
        );
        assert_eq!(serialized[0x7ec..0x800], VOLUME_HEADER_MAGIC);
    }

    #[test]
    fn test_layout_volume_deserialize() {
        let mut volume = [0u8; 0x800];
        volume[0..0x14].copy_from_slice(&VOLUME_HEADER_MAGIC);
        volume[0x7ec..0x800].copy_from_slice(&VOLUME_HEADER_MAGIC);
        volume[0x14] = 10;
        volume[0x18] = 20;

        let volume =
            VolumeDescriptor::deserialize(&volume).expect("Deserialization should succeed");
        assert!(volume.is_valid());
        assert_eq!({ volume.root_table.region.sector }, 10);
        assert_eq!({ volume.root_table.region.size }, 20);
    }
}
