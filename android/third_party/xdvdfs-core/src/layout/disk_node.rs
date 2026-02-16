use bincode::Options;
use serde::{Deserialize, Serialize};

use super::DirectoryEntryDiskData;

/// On-disk representation of a directory entry tree node,
/// including the left and right children, and data.
///
/// Does not include the file name or padding.
#[repr(C)]
#[repr(packed)]
#[derive(Deserialize, Serialize, Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub struct DirectoryEntryDiskNode {
    pub left_entry_offset: u16,
    pub right_entry_offset: u16,
    pub dirent: DirectoryEntryDiskData,
}

impl DirectoryEntryDiskNode {
    pub fn serialize(&self) -> Result<[u8; 0xe], bincode::Error> {
        let mut buffer = [0u8; 0xe];
        self.serialize_into(&mut buffer)?;
        Ok(buffer)
    }

    pub fn serialize_into(&self, buf: &mut [u8]) -> Result<(), bincode::Error> {
        bincode::DefaultOptions::new()
            .with_fixint_encoding()
            .with_little_endian()
            .serialize_into(&mut buf[0..0xe], self)
    }

    pub fn deserialize(buf: &[u8; 0xe]) -> Result<Self, bincode::Error> {
        bincode::DefaultOptions::new()
            .with_fixint_encoding()
            .with_little_endian()
            .deserialize(buf)
    }
}

#[cfg(test)]
mod test {
    use crate::layout::{DirectoryEntryDiskData, DirentAttributes, DiskRegion};

    use super::DirectoryEntryDiskNode;

    #[test]
    fn test_layout_dirent_disk_node_serialize() {
        let node = DirectoryEntryDiskNode {
            left_entry_offset: 257,
            right_entry_offset: 514,
            dirent: DirectoryEntryDiskData {
                data: DiskRegion { sector: 1, size: 2 },
                attributes: DirentAttributes(255),
                filename_length: 7,
            },
        };

        let serialized = node.serialize().expect("Serialization should not fail");
        assert_eq!(serialized[0..2], 257u16.to_le_bytes());
        assert_eq!(serialized[2..4], 514u16.to_le_bytes());
        assert_eq!(serialized[4..8], 1u32.to_le_bytes());
        assert_eq!(serialized[8..12], 2u32.to_le_bytes());
        assert_eq!(serialized[12], 255);
        assert_eq!(serialized[13], 7);
    }

    #[test]
    fn test_layout_dirent_disk_node_deserialize() {
        let serialized: [u8; 0xe] = [1, 1, 2, 2, 1, 0, 0, 0, 2, 0, 0, 0, 255, 7];

        let node = DirectoryEntryDiskNode::deserialize(&serialized)
            .expect("Deserialization should not fail");

        assert_eq!(
            node,
            DirectoryEntryDiskNode {
                left_entry_offset: 257,
                right_entry_offset: 514,
                dirent: DirectoryEntryDiskData {
                    data: DiskRegion { sector: 1, size: 2 },
                    attributes: DirentAttributes(255),
                    filename_length: 7,
                },
            }
        );
    }
}
