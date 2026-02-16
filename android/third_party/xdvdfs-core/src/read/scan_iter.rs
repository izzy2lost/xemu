use crate::blockdev::BlockDeviceRead;
use crate::layout::{
    DirectoryEntryNode, DirectoryEntryTable, SECTOR_SIZE, SECTOR_SIZE_U64, SECTOR_SIZE_USZ,
};
use crate::read::DirectoryEntryReadError;
use maybe_async::maybe_async;

pub struct DirentScanIter<'a, BDR: BlockDeviceRead + ?Sized> {
    sector: usize,
    sector_buf: [u8; SECTOR_SIZE_USZ],
    offset: usize,
    end_sector: usize,
    dev: &'a mut BDR,
}

impl<BDR: BlockDeviceRead + ?Sized> DirentScanIter<'_, BDR> {
    #[maybe_async]
    async fn next_sector(&mut self) -> Result<(), BDR::ReadError> {
        self.offset = 0;
        self.sector += 1;

        if self.sector >= self.end_sector {
            // Don't bother reading sectors in that we don't care about
            return Ok(());
        }

        self.dev
            .read((self.sector as u64) * SECTOR_SIZE_U64, &mut self.sector_buf)
            .await?;

        Ok(())
    }

    #[maybe_async]
    pub async fn next_entry(
        &mut self,
    ) -> Result<Option<DirectoryEntryNode>, DirectoryEntryReadError<BDR::ReadError>> {
        if self.sector >= self.end_sector {
            return Ok(None);
        }

        loop {
            // Invariant: offset must remain in bounds of sector_buf
            assert!(self.offset + 0xe < SECTOR_SIZE_USZ);

            let mut buf = [0; 0xe];
            let name_offset = self.offset + 0xe;
            buf.copy_from_slice(&self.sector_buf[self.offset..name_offset]);

            let img_offset = (self.sector as u64 * SECTOR_SIZE_U64) + self.offset as u64;
            let dirent = DirectoryEntryNode::deserialize(&buf, img_offset)
                .map_err(|_| DirectoryEntryReadError::DeserializationFailed)?;
            let Some(mut dirent) = dirent else {
                // If we find an empty record, but we still have sectors to go,
                // advance the sector count and retry
                if self.sector + 1 < self.end_sector {
                    self.next_sector()
                        .await
                        .map_err(DirectoryEntryReadError::IOError)?;
                    continue;
                }

                break Ok(None);
            };

            let name_len = dirent.node.dirent.filename_length as usize;
            let name_buf = &mut dirent.name[0..name_len];
            assert!(name_offset + name_len <= SECTOR_SIZE_USZ);
            name_buf.copy_from_slice(&self.sector_buf[name_offset..(name_offset + name_len)]);

            // Dirent is valid, advance cursor before returning
            self.offset = name_offset + name_len;
            self.offset += (4 - (self.offset % 4)) % 4;

            if self.offset + 0xe >= SECTOR_SIZE_USZ {
                self.next_sector()
                    .await
                    .map_err(DirectoryEntryReadError::IOError)?;
            }

            break Ok(Some(dirent));
        }
    }
}

#[cfg(feature = "sync")]
impl<E, BDR: BlockDeviceRead<ReadError = E>> Iterator for DirentScanIter<'_, BDR> {
    type Item = DirectoryEntryNode;

    fn next(&mut self) -> Option<Self::Item> {
        self.next_entry().ok().flatten()
    }
}

impl DirectoryEntryTable {
    /// Scan the dirent tree iteratively, reading an entire sector at a time
    /// Order is not guaranteed, but reads are batched
    /// Returns an async iterator that reads sequential records
    #[maybe_async]
    pub async fn scan_dirent_tree<'a, BDR: BlockDeviceRead + ?Sized>(
        &self,
        dev: &'a mut BDR,
    ) -> Result<DirentScanIter<'a, BDR>, DirectoryEntryReadError<BDR::ReadError>> {
        let Ok(sector_offset) = self.offset(0) else {
            // Return a dummy scan iterator that is 0xff filled.
            // This is considered to be an empty sector, and avoids
            // needing to read data or maintain other empty state.
            return Ok(DirentScanIter {
                sector: 0,
                sector_buf: [0xff; SECTOR_SIZE_USZ],
                offset: 0,
                end_sector: 0,
                dev,
            });
        };

        let mut sector_buf = [0; SECTOR_SIZE_USZ];
        dev.read(sector_offset, &mut sector_buf)
            .await
            .map_err(DirectoryEntryReadError::IOError)?;

        let sector = self.region.sector as usize;
        Ok(DirentScanIter {
            sector,
            sector_buf,
            offset: 0,
            end_sector: sector + self.region.size.div_ceil(SECTOR_SIZE) as usize,
            dev,
        })
    }
}

#[cfg(all(test, feature = "write"))]
mod test {
    use futures::executor::block_on;

    use crate::{
        blockdev::BlockDeviceWrite,
        layout::{
            DirectoryEntryDiskData, DirectoryEntryDiskNode, DirectoryEntryNode,
            DirectoryEntryTable, DirentAttributes, DiskRegion,
        },
        read::DirentScanIter,
        write::{
            dirtab::{AvlDirectoryEntryTableBuilder, DirtabWriterBuffers},
            fs::{
                MemoryFilesystem, SectorLinearBlockDevice, SectorLinearBlockFilesystem,
                SectorLinearImage,
            },
            sector::SectorAllocator,
        },
    };

    fn name_bytes_from(name: &str) -> [u8; 256] {
        assert!(name.len() <= 256);
        let mut out = [0u8; 256];
        out[..name.len()].copy_from_slice(name.as_bytes());
        out
    }

    #[test]
    fn test_read_scan_iter_empty_size() {
        let table = DirectoryEntryTable {
            region: DiskRegion { sector: 0, size: 0 },
        };
        let mut dev = [];

        let mut scan_iter = block_on(table.scan_dirent_tree(dev.as_mut_slice()))
            .expect("Creating scan iter should not fail");
        assert!(matches!(
            scan_iter,
            DirentScanIter {
                sector: 0,
                offset: 0,
                end_sector: 0,
                ..
            }
        ));

        let next = block_on(scan_iter.next_entry());
        assert_eq!(next, Ok(None));
    }

    #[test]
    fn test_read_scan_iter_empty_filled() {
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 2048,
            },
        };
        let mut dev = [0xffu8; 2048];

        let mut scan_iter = block_on(table.scan_dirent_tree(dev.as_mut_slice()))
            .expect("Creating scan iter should not fail");
        assert!(matches!(
            scan_iter,
            DirentScanIter {
                sector: 0,
                offset: 0,
                end_sector: 1,
                ..
            }
        ));

        let next = block_on(scan_iter.next_entry());
        assert_eq!(next, Ok(None));
    }

    #[test]
    fn test_read_scan_iter_skip_null_entry() {
        let mut slbd = SectorLinearBlockDevice::default();
        let mut fs = SectorLinearBlockFilesystem::new(MemoryFilesystem::default());

        {
            #[rustfmt::skip]
            let dirtab = [
                0, 0, 0, 0,
                1, 0, 0, 0,
                2, 0, 0, 0,
                0xff, 1, 'A' as u8,
                0xff, // padding
                0, 0, 0, 0,
                1, 0, 0, 0,
                2, 0, 0, 0,
                0xff, 1, 'B' as u8,
                0xff, // padding
            ];
            block_on(slbd.write(2048, &dirtab)).expect("write should succeed");
        }

        {
            #[rustfmt::skip]
            let dirtab = [
                0, 0, 0, 0,
                1, 0, 0, 0,
                2, 0, 0, 0,
                0xff, 1, 'C' as u8,
                0xff, // padding
            ];
            block_on(slbd.write(4096, &dirtab)).expect("write should succeed");
            block_on(slbd.write(6144, &dirtab)).expect("write should succeed");
        }

        let mut dev = SectorLinearImage::new(&slbd, &mut fs);
        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 1,
                size: 4096,
            },
        };

        let mut iter =
            block_on(table.scan_dirent_tree(&mut dev)).expect("Creating scan iter should succeed");

        let next = block_on(iter.next_entry()).expect("Error on next entry is unexpected");
        assert_eq!(
            next,
            Some(DirectoryEntryNode {
                node: DirectoryEntryDiskNode {
                    left_entry_offset: 0,
                    right_entry_offset: 0,
                    dirent: DirectoryEntryDiskData {
                        data: DiskRegion { sector: 1, size: 2 },
                        attributes: DirentAttributes(0xff),
                        filename_length: 1,
                    },
                },
                name: name_bytes_from("A"),
                offset: 2048 + 0x00,
            })
        );

        let next = block_on(iter.next_entry()).expect("Error on next entry is unexpected");
        assert_eq!(
            next,
            Some(DirectoryEntryNode {
                node: DirectoryEntryDiskNode {
                    left_entry_offset: 0,
                    right_entry_offset: 0,
                    dirent: DirectoryEntryDiskData {
                        data: DiskRegion { sector: 1, size: 2 },
                        attributes: DirentAttributes(0xff),
                        filename_length: 1,
                    },
                },
                name: name_bytes_from("B"),
                offset: 2048 + 0x10,
            })
        );

        let next = block_on(iter.next_entry()).expect("Error on next entry is unexpected");
        assert_eq!(
            next,
            Some(DirectoryEntryNode {
                node: DirectoryEntryDiskNode {
                    left_entry_offset: 0,
                    right_entry_offset: 0,
                    dirent: DirectoryEntryDiskData {
                        data: DiskRegion { sector: 1, size: 2 },
                        attributes: DirentAttributes(0xff),
                        filename_length: 1,
                    },
                },
                name: name_bytes_from("C"),
                offset: 4096 + 0x00,
            })
        );

        // Ensure reads don't extend into subsequent sectors
        let next = block_on(iter.next_entry()).expect("Error on next entry is unexpected");
        assert_eq!(next, None);
        let next = block_on(iter.next_entry()).expect("Error on next entry is unexpected");
        assert_eq!(next, None);
    }

    #[test]
    #[cfg(feature = "std")]
    fn test_read_scan_iter_sector_rollover() {
        use alloc::string::String;
        use alloc::vec::Vec;
        use std::collections::HashSet;

        let mut entry_names: Vec<String> = Vec::new();

        // Each entry is 0x10 (filename length of 2),
        // so 256 fills two sectors exactly.
        for i in 0..256 {
            entry_names.push(alloc::format!("{i:02x}"));
        }

        let mut dtb = AvlDirectoryEntryTableBuilder::default();
        for name in &entry_names {
            dtb.add_file(&name, 10)
                .expect("Dirent insertion should succeed");
        }
        let mut dtb = dtb.build().expect("Building dirtab should succeed");
        let mut allocator = SectorAllocator::default();
        let mut buffers = DirtabWriterBuffers::default();
        dtb.disk_repr(&mut allocator, &mut buffers)
            .expect("Serializing dirtab should succeed");
        assert_eq!(buffers.dirtab_bytes.len(), 4096);

        let table = DirectoryEntryTable {
            region: DiskRegion {
                sector: 0,
                size: 4096,
            },
        };

        let mut iter = block_on(table.scan_dirent_tree(buffers.dirtab_bytes.as_mut_slice()))
            .expect("Creating iter should succeed");

        let mut name_set = HashSet::new();
        for name in &entry_names {
            name_set.insert(name.as_str());
        }

        for _ in 0..256 {
            let next = block_on(iter.next_entry())
                .expect("Reading next entry should succeed")
                .expect("Next entry should exist");

            // The order of entries in the array is not guaranteed,
            // so use a set to ensure all names are pulled.
            let name_str = next.name_str().expect("Name should be deserializable");
            assert!(name_set.remove(name_str.as_ref()));
        }

        assert!(name_set.is_empty());

        let next = block_on(iter.next_entry()).expect("Reading next entry should succeed");
        assert_eq!(next, None);
        let next = block_on(iter.next_entry()).expect("Reading next entry should succeed");
        assert_eq!(next, None);
    }
}
