use alloc::borrow::Cow;
use encoding_rs::WINDOWS_1252;
use thiserror::Error;

use super::DirectoryEntryDiskNode;

/// In-memory structure to contain an on-disk tree node and
/// file name information.
#[derive(Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub struct DirectoryEntryNode {
    pub node: DirectoryEntryDiskNode,
    pub name: [u8; 256],
    pub offset: u64,
}

#[derive(Error, Debug, Copy, Clone, Eq, PartialEq)]
#[error("failed to deserialize file name into utf-8")]
pub struct NameDeserializationError;

impl DirectoryEntryNode {
    /// Deserializes a DirectoryEntryNode from a buffer
    /// containing a DirectoryEntryDiskNode. This does not
    /// populate the dirent name. Returns None if the node
    /// is empty.
    pub fn deserialize(
        dirent_buf: &[u8; 0xe],
        offset: u64,
    ) -> Result<Option<Self>, bincode::Error> {
        // Empty directory entries are filled with 0xff or 0x00
        if dirent_buf == &[0xff; 0xe] || dirent_buf == &[0x00; 0xe] {
            return Ok(None);
        }

        let node = DirectoryEntryDiskNode::deserialize(dirent_buf)?;
        Ok(Some(Self {
            node,
            name: [0; 256],
            offset,
        }))
    }

    pub fn name_slice(&self) -> &[u8] {
        let name_len = self.node.dirent.filename_length as usize;
        &self.name[0..name_len]
    }

    /// Returns a UTF-8 encoded representation of the file name
    /// If the filename cannot be reencoded into UTF-8, returns None
    pub fn name_str(&self) -> Result<Cow<'_, str>, NameDeserializationError> {
        let name_bytes = self.name_slice();
        WINDOWS_1252
            .decode_without_bom_handling_and_without_replacement(name_bytes)
            .ok_or(NameDeserializationError)
    }
}

#[cfg(test)]
mod test {
    use crate::layout::{
        DirectoryEntryDiskData, DirectoryEntryDiskNode, DirentAttributes, DiskRegion,
    };

    use super::DirectoryEntryNode;

    #[test]
    fn test_layout_dirent_node_deserialize_empty_ff_filled() {
        let dirent_buf = [0xff; 0xe];
        let dirent_node = DirectoryEntryNode::deserialize(&dirent_buf, 10);
        assert!(dirent_node.is_ok_and(|node| node == None));
    }

    #[test]
    fn test_layout_dirent_node_deserialize_empty_zero_filled() {
        let dirent_buf = [0x00; 0xe];
        let dirent_node = DirectoryEntryNode::deserialize(&dirent_buf, 10);
        assert!(dirent_node.is_ok_and(|node| node == None));
    }

    #[test]
    fn test_layout_dirent_node_deserialize_entry() {
        let mut dirent_buf = [0x00; 0xe];
        dirent_buf[0] = 1;
        dirent_buf[1] = 1;
        dirent_buf[2] = 2;
        dirent_buf[3] = 2;
        dirent_buf[4] = 3;
        dirent_buf[8] = 4;
        dirent_buf[12] = 0;
        dirent_buf[13] = 5;

        let dirent_node = DirectoryEntryNode::deserialize(&dirent_buf, 10)
            .expect("Node should deserialize correctly")
            .expect("Node should be present");
        assert_eq!({ dirent_node.node.left_entry_offset }, 257);
        assert_eq!({ dirent_node.node.right_entry_offset }, 514);
        assert_eq!({ dirent_node.node.dirent.data.sector }, 3);
        assert_eq!({ dirent_node.node.dirent.data.size }, 4);
        assert_eq!(dirent_node.node.dirent.attributes.attrs(), 0);
        assert_eq!(dirent_node.node.dirent.filename_length, 5);
        assert_eq!(dirent_node.offset, 10);
    }

    #[test]
    fn test_layout_dirent_node_name_decode_windows_1252() {
        let mut name = [0u8; 256];
        name[0] = 'A' as u8;
        name[1] = 159; // Ÿ

        let node = DirectoryEntryNode {
            node: DirectoryEntryDiskNode {
                left_entry_offset: 0,
                right_entry_offset: 0,
                dirent: DirectoryEntryDiskData {
                    data: DiskRegion {
                        size: 10,
                        sector: 0,
                    },
                    attributes: DirentAttributes(0),
                    filename_length: 2,
                },
            },
            name,
            offset: 0,
        };

        let decoded_name = node.name_str();
        assert_eq!(decoded_name, Ok("AŸ".into()));
    }
}
