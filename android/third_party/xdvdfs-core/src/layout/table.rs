use serde::{Deserialize, Serialize};

use crate::layout::OutOfBounds;

use super::DiskRegion;

/// A DiskRegion that contains a directory entry table structure.
///
/// This differentiates regions that contain file data.
#[repr(C)]
#[repr(packed)]
#[derive(Deserialize, Serialize, Debug, Copy, Clone, Eq, PartialEq)]
pub struct DirectoryEntryTable {
    pub region: DiskRegion,
}

impl DirectoryEntryTable {
    pub fn new(size: u32, sector: u32) -> Self {
        Self {
            region: DiskRegion { size, sector },
        }
    }

    pub fn is_empty(&self) -> bool {
        self.region.is_empty()
    }

    pub fn offset(&self, offset: u64) -> Result<u64, OutOfBounds> {
        self.region.offset(offset)
    }
}

#[cfg(test)]
mod test {
    use crate::layout::{DirectoryEntryTable, OutOfBounds};

    use super::DiskRegion;

    #[test]
    fn test_layout_dirent_table_empty() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 10,
                size: 0,
            },
        };

        assert!(table.is_empty());
    }

    #[test]
    fn test_layout_dirent_table_non_empty() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 10,
                size: 2048,
            },
        };

        assert!(!table.is_empty());
    }

    #[test]
    fn test_layout_dirent_table_offset_out_of_bounds() {
        let table = DirectoryEntryTable {
            region: DiskRegion { sector: 3, size: 7 },
        };

        let res = table.offset(11);
        assert_eq!(
            res,
            Err(OutOfBounds {
                offset: 11,
                size: 7
            })
        );
    }

    #[test]
    fn test_layout_dirent_table_offset_in_bounds() {
        let table = DirectoryEntryTable {
            region: DiskRegion { sector: 3, size: 7 },
        };

        let res = table.offset(5);
        assert_eq!(res, Ok(6149));
    }
}
