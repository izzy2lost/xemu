use serde::{Deserialize, Serialize};

use super::{DirectoryEntryTable, DirentAttributes, DiskRegion};

/// On-disk representation of a directory entry tree data,
/// excluding the left and right children.
///
/// Does not include the file name or padding.
#[repr(C)]
#[repr(packed)]
#[derive(Deserialize, Serialize, Debug, Copy, Clone, Eq, PartialEq, Hash)]
pub struct DirectoryEntryDiskData {
    pub data: DiskRegion,
    pub attributes: DirentAttributes,
    pub filename_length: u8,
}

impl DirectoryEntryDiskData {
    pub fn is_empty(&self) -> bool {
        self.data.is_empty()
    }

    pub fn is_directory(&self) -> bool {
        self.attributes.directory()
    }

    pub fn dirent_table(&self) -> Option<DirectoryEntryTable> {
        if self.is_directory() {
            Some(DirectoryEntryTable { region: self.data })
        } else {
            None
        }
    }
}

#[cfg(test)]
mod test {
    use crate::layout::DirectoryEntryTable;

    use super::{DirectoryEntryDiskData, DirentAttributes, DiskRegion};

    #[test]
    fn test_layout_dirent_disk_data_empty() {
        let dirent = DirectoryEntryDiskData {
            data: DiskRegion { sector: 0, size: 0 },
            attributes: DirentAttributes(0),
            filename_length: 0,
        };

        assert!(dirent.is_empty());
    }

    #[test]
    fn test_layout_dirent_disk_data_non_empty() {
        let dirent = DirectoryEntryDiskData {
            data: DiskRegion { sector: 0, size: 1 },
            attributes: DirentAttributes(0),
            filename_length: 0,
        };

        assert!(!dirent.is_empty());
    }

    #[test]
    fn test_layout_dirent_disk_data_directory() {
        let dirent = DirectoryEntryDiskData {
            data: DiskRegion { sector: 2, size: 1 },
            attributes: DirentAttributes(0).with_directory(true),
            filename_length: 0,
        };

        assert!(dirent.is_directory());
        assert_eq!(
            dirent.dirent_table(),
            Some(DirectoryEntryTable {
                region: DiskRegion { sector: 2, size: 1 },
            })
        );
    }

    #[test]
    fn test_layout_dirent_disk_data_file() {
        let dirent = DirectoryEntryDiskData {
            data: DiskRegion { sector: 2, size: 1 },
            attributes: DirentAttributes(0).with_directory(false),
            filename_length: 0,
        };

        assert!(!dirent.is_directory());
        assert_eq!(dirent.dirent_table(), None);
    }
}
