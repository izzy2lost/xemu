use crate::blockdev::BlockDeviceRead;
use crate::layout::{cmp_ignore_case_utf8, DirectoryEntryNode, DirectoryEntryTable};
use crate::read::{DirectoryTableLookupError, DirectoryTableWalkError};
use maybe_async::maybe_async;

#[derive(Copy, Clone, Debug, Eq, PartialEq)]
enum DirentSearchResult {
    Found,
    NextOffset(u64),
}

impl DirectoryEntryTable {
    fn dirent_search_next_offset<E>(
        &self,
        dirent: &DirectoryEntryNode,
        name: &str,
    ) -> Result<DirentSearchResult, DirectoryTableLookupError<E>> {
        let dirent_name = dirent.name_str()?;
        debugln!("[find_dirent] Node name {}", dirent_name);

        let cmp = cmp_ignore_case_utf8(name, &dirent_name);
        debugln!("[find_dirent] Comparison result: {:?}", cmp);

        let next_offset = match cmp {
            core::cmp::Ordering::Equal => return Ok(DirentSearchResult::Found),
            core::cmp::Ordering::Less => dirent.node.left_entry_offset,
            core::cmp::Ordering::Greater => dirent.node.right_entry_offset,
        };

        if next_offset == 0 || next_offset == 0xff {
            return Err(DirectoryTableLookupError::DoesNotExist);
        }

        let next_offset = self.offset(4 * next_offset as u64)?;
        Ok(DirentSearchResult::NextOffset(next_offset))
    }

    #[maybe_async]
    async fn find_dirent<BDR: BlockDeviceRead + ?Sized>(
        &self,
        dev: &mut BDR,
        name: &str,
    ) -> Result<DirectoryEntryNode, DirectoryTableLookupError<BDR::ReadError>> {
        debugln!("[find_dirent] Called on {}", name);
        if self.is_empty() {
            return Err(DirectoryTableLookupError::DirectoryEmpty);
        }

        let mut offset = self.offset(0)?;
        loop {
            let dirent = DirectoryEntryNode::read_from_disk(dev, offset).await?;
            let dirent = dirent.ok_or(DirectoryTableLookupError::DoesNotExist)?;
            traceln!("[find_dirent] Found node: {:?}", dirent.node);

            match self.dirent_search_next_offset(&dirent, name)? {
                DirentSearchResult::Found => break Ok(dirent),
                DirentSearchResult::NextOffset(next_offset) => offset = next_offset,
            }
        }
    }

    /// Retrieves the directory entry node corresponding to the provided path,
    /// if it exists.
    ///
    /// Returns None if the root path is provided (root has no dirent)
    /// or the path does not exist.
    #[maybe_async]
    pub async fn walk_path<BDR: BlockDeviceRead + ?Sized>(
        &self,
        dev: &mut BDR,
        path: &str,
    ) -> Result<DirectoryEntryNode, DirectoryTableLookupError<BDR::ReadError>> {
        debugln!("[walk_path] Called on {}", path);
        if path.is_empty() || path == "/" {
            return Err(DirectoryTableLookupError::NoDirent);
        }

        let mut dirent_tab = *self;
        let mut path_iter = path
            .trim_start_matches('/')
            .split_terminator('/')
            .peekable();

        while let Some(segment) = path_iter.next() {
            let dirent = dirent_tab.find_dirent(dev, segment).await?;
            debugln!("[walk_path] Found dirent: {}", dirent.name_str()?);
            traceln!("[walk_path] Node: {:?}", dirent.node);
            let dirent_data = &dirent.node.dirent;

            if path_iter.peek().is_none() {
                return Ok(dirent);
            }

            dirent_tab = dirent_data
                .dirent_table()
                .ok_or(DirectoryTableLookupError::IsNotDirectory)?;
        }

        unreachable!("path_iter has been consumed without returning last dirent")
    }

    /// Walks the directory entry table in preorder, returning all directory entries.
    #[maybe_async]
    pub async fn walk_dirent_tree<BDR: BlockDeviceRead + ?Sized>(
        &self,
        dev: &mut BDR,
    ) -> Result<alloc::vec::Vec<DirectoryEntryNode>, DirectoryTableWalkError<BDR::ReadError>> {
        use alloc::vec;

        debugln!("[walk_dirent_tree] {:?}", self);

        let mut dirents = vec![];
        if self.is_empty() {
            return Ok(dirents);
        }

        let mut stack = vec![0];
        while let Some(top) = stack.pop() {
            let offset = self.offset(top)?;
            let dirent = DirectoryEntryNode::read_from_disk(dev, offset).await?;

            if let Some(dirent) = dirent {
                debugln!(
                    "[walk_dirent_tree] Found dirent \"{}\"",
                    dirent.name_str().unwrap_or("<invalid dirent name>".into())
                );
                traceln!("[walk_dirent_tree] Node: {:?} at offset {}", dirent, top);

                let right_child = dirent.node.right_entry_offset;
                if right_child != 0 && right_child != 0xffff {
                    stack.push(4 * dirent.node.right_entry_offset as u64);
                }

                let left_child = dirent.node.left_entry_offset;
                if left_child != 0 && left_child != 0xffff {
                    stack.push(4 * dirent.node.left_entry_offset as u64);
                }

                dirents.push(dirent);
            }
        }

        Ok(dirents)
    }

    #[maybe_async]
    pub async fn file_tree<BDR: BlockDeviceRead + ?Sized>(
        &self,
        dev: &mut BDR,
    ) -> Result<
        alloc::vec::Vec<(alloc::string::String, DirectoryEntryNode)>,
        DirectoryTableWalkError<BDR::ReadError>,
    > {
        use alloc::format;
        use alloc::string::String;
        use alloc::vec;

        let mut dirents = vec![];

        let mut stack = vec![(String::from(""), *self)];
        while let Some((parent, tree)) = stack.pop() {
            debugln!("[file_tree] Descending through {}", parent);
            let children = tree.walk_dirent_tree(dev).await?;
            for child in children {
                if let Some(dirent_table) = child.node.dirent.dirent_table() {
                    let child_name = child.name_str()?;
                    stack.push((format!("{parent}/{child_name}"), dirent_table));
                }

                dirents.push((parent.clone(), child));
            }
        }

        Ok(dirents)
    }
}

#[cfg(test)]
mod test {
    use futures::executor::block_on;

    use crate::{
        layout::{
            DirectoryEntryDiskData, DirectoryEntryDiskNode, DirectoryEntryNode,
            DirectoryEntryTable, DirentAttributes, DiskRegion,
        },
        read::{dirent_table::DirentSearchResult, DirectoryTableLookupError},
    };

    pub fn name_bytes_from(name: &str) -> [u8; 256] {
        assert!(name.len() <= 256);
        let mut out = [0u8; 256];
        out[..name.len()].copy_from_slice(name.as_bytes());
        out
    }

    #[test]
    fn test_read_dirtab_dirent_next_offset_found_match() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 4096,
            },
        };

        let dirent = DirectoryEntryNode {
            node: DirectoryEntryDiskNode {
                left_entry_offset: 257,
                right_entry_offset: 514,
                dirent: DirectoryEntryDiskData {
                    data: DiskRegion { sector: 1, size: 2 },
                    attributes: DirentAttributes(255),
                    filename_length: 2,
                },
            },
            name: name_bytes_from("Bc"),
            offset: 0,
        };

        let res = table
            .dirent_search_next_offset::<()>(&dirent, "Bc")
            .expect("Dirent search should succeed");
        let mut dirent_name = [0u8; 256];
        dirent_name[0] = 'B' as u8;
        dirent_name[1] = 'c' as u8;
        assert_eq!(res, DirentSearchResult::Found);
    }

    #[test]
    fn test_read_dirtab_dirent_next_offset_left_child() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 4096,
            },
        };

        let dirent = DirectoryEntryNode {
            node: DirectoryEntryDiskNode {
                left_entry_offset: 257,
                right_entry_offset: 514,
                dirent: DirectoryEntryDiskData {
                    data: DiskRegion { sector: 1, size: 2 },
                    attributes: DirentAttributes(255),
                    filename_length: 2,
                },
            },
            name: name_bytes_from("Bc"),
            offset: 0,
        };

        let res = table
            .dirent_search_next_offset::<()>(&dirent, "ba")
            .expect("Dirent search should succeed");
        assert_eq!(res, DirentSearchResult::NextOffset(4 * 257));
    }

    #[test]
    fn test_read_dirtab_dirent_next_offset_right_child() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 4096,
            },
        };

        let dirent = DirectoryEntryNode {
            node: DirectoryEntryDiskNode {
                left_entry_offset: 257,
                right_entry_offset: 514,
                dirent: DirectoryEntryDiskData {
                    data: DiskRegion { sector: 1, size: 2 },
                    attributes: DirentAttributes(255),
                    filename_length: 2,
                },
            },
            name: name_bytes_from("Bc"),
            offset: 0,
        };

        let res = table
            .dirent_search_next_offset::<()>(&dirent, "bd")
            .expect("Dirent search should succeed");
        assert_eq!(res, DirentSearchResult::NextOffset(4 * 514));
    }

    #[test]
    fn test_read_dirtab_dirent_next_offset_zero_left_child_does_not_exist() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 4096,
            },
        };

        let dirent = DirectoryEntryNode {
            node: DirectoryEntryDiskNode {
                left_entry_offset: 0,
                right_entry_offset: 514,
                dirent: DirectoryEntryDiskData {
                    data: DiskRegion { sector: 1, size: 2 },
                    attributes: DirentAttributes(255),
                    filename_length: 2,
                },
            },
            name: name_bytes_from("Bc"),
            offset: 0,
        };

        let res = table.dirent_search_next_offset::<()>(&dirent, "ba");
        assert_eq!(res, Err(DirectoryTableLookupError::DoesNotExist));
    }

    #[test]
    fn test_read_dirtab_dirent_next_offset_zero_right_child_does_not_exist() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 4096,
            },
        };

        let dirent = DirectoryEntryNode {
            node: DirectoryEntryDiskNode {
                left_entry_offset: 257,
                right_entry_offset: 0,
                dirent: DirectoryEntryDiskData {
                    data: DiskRegion { sector: 1, size: 2 },
                    attributes: DirentAttributes(255),
                    filename_length: 2,
                },
            },
            name: name_bytes_from("Bc"),
            offset: 0,
        };

        let res = table.dirent_search_next_offset::<()>(&dirent, "bd");
        assert_eq!(res, Err(DirectoryTableLookupError::DoesNotExist));
    }

    #[test]
    fn test_read_dirtab_dirent_next_offset_ff_left_child_does_not_exist() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 4096,
            },
        };

        let dirent = DirectoryEntryNode {
            node: DirectoryEntryDiskNode {
                left_entry_offset: 0xff,
                right_entry_offset: 514,
                dirent: DirectoryEntryDiskData {
                    data: DiskRegion { sector: 1, size: 2 },
                    attributes: DirentAttributes(255),
                    filename_length: 2,
                },
            },
            name: name_bytes_from("Bc"),
            offset: 0,
        };

        let res = table.dirent_search_next_offset::<()>(&dirent, "ba");
        assert_eq!(res, Err(DirectoryTableLookupError::DoesNotExist));
    }

    #[test]
    fn test_read_dirtab_dirent_next_offset_ff_right_child_does_not_exist() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 4096,
            },
        };

        let dirent = DirectoryEntryNode {
            node: DirectoryEntryDiskNode {
                left_entry_offset: 257,
                right_entry_offset: 0xff,
                dirent: DirectoryEntryDiskData {
                    data: DiskRegion { sector: 1, size: 2 },
                    attributes: DirentAttributes(255),
                    filename_length: 2,
                },
            },
            name: name_bytes_from("Bc"),
            offset: 0,
        };

        let res = table.dirent_search_next_offset::<()>(&dirent, "bd");
        assert_eq!(res, Err(DirectoryTableLookupError::DoesNotExist));
    }

    #[test]
    fn test_read_dirtab_find_dirent_empty_zero_size() {
        let table = DirectoryEntryTable {
            region: DiskRegion { sector: 0, size: 0 },
        };

        let mut dev = [];
        let res = block_on(table.find_dirent(dev.as_mut_slice(), "bd"));
        assert_eq!(res, Err(DirectoryTableLookupError::DirectoryEmpty));
    }

    #[test]
    fn test_read_dirtab_find_dirent_ff_filled_sector() {
        let mut dev = [0xffu8; 2048];
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 2048,
            },
        };

        let res = block_on(table.find_dirent(dev.as_mut_slice(), "bd"));
        assert_eq!(res, Err(DirectoryTableLookupError::DoesNotExist));
    }

    #[test]
    fn test_read_dirtab_find_dirent_empty_filled_sector() {
        let mut dev = [0u8; 2048];
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 2048,
            },
        };

        let res = block_on(table.find_dirent(dev.as_mut_slice(), "bd"));
        assert_eq!(res, Err(DirectoryTableLookupError::DoesNotExist));
    }

    #[test]
    fn test_read_dirtab_find_dirent_tree_search_succeeds() {
        #[rustfmt::skip]
        let mut dev = [
            0, 0, 4, 0, // Right child -> BE
            1, 0, 0, 0,
            2, 0, 0, 0,
            0xff, 2, 'B' as u8, 'c' as u8,
            8, 0, 0, 0, // Left child -> Bd
            3, 0, 0, 0,
            4, 0, 0, 0,
            0xff, 2, 'B' as u8, 'E' as u8,
            0, 0, 0, 0,
            5, 0, 0, 0,
            6, 0, 0, 0,
            0xff, 2, 'B' as u8, 'd' as u8,
        ];
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 2048,
            },
        };

        let res =
            block_on(table.find_dirent(dev.as_mut_slice(), "bd")).expect("Search should succeed");
        let mut dirent_name = [0u8; 256];
        dirent_name[0] = 'B' as u8;
        dirent_name[1] = 'd' as u8;
        assert_eq!(
            res,
            DirectoryEntryNode {
                node: DirectoryEntryDiskNode {
                    left_entry_offset: 0,
                    right_entry_offset: 0,
                    dirent: DirectoryEntryDiskData {
                        data: DiskRegion { sector: 5, size: 6 },
                        attributes: DirentAttributes(255),
                        filename_length: 2,
                    },
                },
                name: name_bytes_from("Bd"),
                offset: 0x20,
            }
        );
    }

    #[test]
    fn test_read_dirtab_walk_path_root() {
        let mut dev = [];
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 2048,
            },
        };

        let res = block_on(table.walk_path(dev.as_mut_slice(), "/"));
        assert_eq!(res, Err(DirectoryTableLookupError::NoDirent));
    }

    #[test]
    fn test_read_dirtab_walk_dirent_tree_empty_zero_size() {
        let table = DirectoryEntryTable {
            region: DiskRegion { sector: 0, size: 0 },
        };

        let mut dev = [];
        let res = block_on(table.walk_dirent_tree(dev.as_mut_slice()))
            .expect("Dirtree walk should succeed");
        assert_eq!(&res, &[]);
    }

    #[test]
    fn test_read_dirtab_walk_dirent_tree_empty_zero_filled() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 2048,
            },
        };

        let mut dev = [0u8; 2048];
        let res = block_on(table.walk_dirent_tree(dev.as_mut_slice()))
            .expect("Dirtree walk should succeed");
        assert_eq!(&res, &[]);
    }

    #[test]
    fn test_read_dirtab_walk_dirent_tree_returns_entries_in_preorder() {
        #[rustfmt::skip]
        let mut dev = [
            // Offset 0x00 (0)
            12, 0, 4, 0, // Left child -> Aa, Right child -> Bf
            1, 0, 0, 0,
            2, 0, 0, 0,
            0xff, 2, 'B' as u8, 'c' as u8,
            // Offset 0x10 (4)
            8, 0, 0, 0, // Left child -> Bd
            3, 0, 0, 0,
            4, 0, 0, 0,
            0xff, 2, 'B' as u8, 'f' as u8,
            // Offset 0x20 (8)
            0, 0, 16, 0, // Right child -> Be
            5, 0, 0, 0,
            6, 0, 0, 0,
            0xff, 2, 'B' as u8, 'd' as u8,
            // Offset 0x30 (12)
            0, 0, 0, 0,
            7, 0, 0, 0,
            8, 0, 0, 0,
            0xff, 2, 'A' as u8, 'a' as u8,
            // Offset 0x40 (16)
            0xff, 0xff, 0xff, 0xff,
            9, 0, 0, 0,
            10, 0, 0, 0,
            0xff, 2, 'B' as u8, 'e' as u8,
        ];
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 2048,
            },
        };

        let res = block_on(table.walk_dirent_tree(dev.as_mut_slice()))
            .expect("Dirtree walk should succeed");

        fn dirent_node(
            name: &str,
            offset: u64,
            lchild: u16,
            rchild: u16,
            sector: u32,
            size: u32,
        ) -> DirectoryEntryNode {
            DirectoryEntryNode {
                node: DirectoryEntryDiskNode {
                    left_entry_offset: lchild,
                    right_entry_offset: rchild,
                    dirent: DirectoryEntryDiskData {
                        data: DiskRegion { sector, size },
                        attributes: DirentAttributes(0xff),
                        filename_length: name.len() as u8,
                    },
                },
                name: name_bytes_from(name),
                offset,
            }
        }
        assert_eq!(res.len(), 5);
        assert_eq!(res[0], dirent_node("Bc", 0x00, 12, 4, 1, 2));
        assert_eq!(res[1], dirent_node("Aa", 0x30, 0, 0, 7, 8));
        assert_eq!(res[2], dirent_node("Bf", 0x10, 8, 0, 3, 4));
        assert_eq!(res[3], dirent_node("Bd", 0x20, 0, 16, 5, 6));
        assert_eq!(res[4], dirent_node("Be", 0x40, 0xffff, 0xffff, 9, 10));
    }
}

#[cfg(all(test, feature = "write"))]
mod test_with_write {
    use futures::executor::block_on;

    use super::test::name_bytes_from;
    use crate::{
        blockdev::BlockDeviceWrite,
        layout::{
            DirectoryEntryDiskData, DirectoryEntryDiskNode, DirectoryEntryNode,
            DirectoryEntryTable, DirentAttributes, DiskRegion, SECTOR_SIZE_U64,
        },
        write::{
            fs::{
                MemoryFilesystem, SectorLinearBlockDevice, SectorLinearBlockFilesystem,
                SectorLinearImage,
            },
            img::NoOpProgressVisitor,
        },
    };

    #[test]
    fn test_read_dirtab_walk_path_multiple() {
        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(MemoryFilesystem::default());

        let mut push_table = |sector: u8, first_char: char| {
            #[rustfmt::skip]
            let table_bytes = [
                0, 0, 4, 0, // Right child -> BE
                sector + 1, 0, 0, 0,
                0, 8, 0, 0,
                0xff, 2, first_char as u8, 'c' as u8,
                8, 0, 0, 0, // Left child -> Bd
                sector + 1, 0, 0, 0,
                0, 8, 0, 0,
                0xff, 2, first_char as u8, 'E' as u8,
                0, 0, 0, 0,
                sector + 1, 0, 0, 0,
                0, 8, 0, 0,
                0xff, 2, first_char as u8, 'd' as u8,
            ];

            block_on(slbd.write(sector as u64 * SECTOR_SIZE_U64, &table_bytes))
                .expect("Write should succeed");
        };

        push_table(33, 'B');
        push_table(34, 'C');
        push_table(35, 'A');

        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 33,
                size: 2048,
            },
        };

        let mut dev = SectorLinearImage::new(&slbd, &mut slbfs);

        let res =
            block_on(table.walk_path(&mut dev, "/Bd/CE/Ac")).expect("Path walk should succeed");
        let mut dirent_name = [0u8; 256];
        dirent_name[0] = 'A' as u8;
        dirent_name[1] = 'c' as u8;
        assert_eq!(
            res,
            DirectoryEntryNode {
                node: DirectoryEntryDiskNode {
                    left_entry_offset: 0,
                    right_entry_offset: 4,
                    dirent: DirectoryEntryDiskData {
                        data: DiskRegion {
                            sector: 36,
                            size: 2048,
                        },
                        attributes: DirentAttributes(255),
                        filename_length: 2,
                    },
                },
                name: name_bytes_from("Ac"),
                offset: 35 * SECTOR_SIZE_U64,
            }
        );
    }

    #[test]
    fn test_read_dirtab_file_tree() {
        let mut fs = MemoryFilesystem::default();
        fs.touch("/a/b/c");
        fs.touch("/a/b/d");
        fs.touch("/a/e");
        fs.touch("/f");

        let mut slbd = SectorLinearBlockDevice::default();
        let mut fs = SectorLinearBlockFilesystem::new(fs);
        block_on(crate::write::img::create_xdvdfs_image(
            &mut fs,
            &mut slbd,
            NoOpProgressVisitor,
        ))
        .expect("Image creation should succeed");
        let mut dev = SectorLinearImage::new(&slbd, &mut fs);

        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 33,
                size: 2048,
            },
        };
        let tree = block_on(table.file_tree(&mut dev)).expect("File tree should not fail");
        assert_eq!(tree.len(), 6);

        assert_eq!(&tree[0].0, "");
        assert_eq!(tree[0].1.name, name_bytes_from("a"));
        assert_eq!(&tree[1].0, "");
        assert_eq!(tree[1].1.name, name_bytes_from("f"));
        assert_eq!(&tree[2].0, "/a");
        assert_eq!(tree[2].1.name, name_bytes_from("b"));
        assert_eq!(&tree[3].0, "/a");
        assert_eq!(tree[3].1.name, name_bytes_from("e"));
        assert_eq!(&tree[4].0, "/a/b");
        assert_eq!(tree[4].1.name, name_bytes_from("c"));
        assert_eq!(&tree[5].0, "/a/b");
        assert_eq!(tree[5].1.name, name_bytes_from("d"));
    }
}
