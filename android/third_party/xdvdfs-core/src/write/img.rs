use crate::blockdev::BlockDeviceWrite;
use crate::layout;
use crate::write::{fs, sector};

use alloc::vec;
use alloc::vec::Vec;

use super::dirtab::{
    AvlDirectoryEntryTableBuilder, AvlDirectoryEntryTableWriter, DirtabWriterBuffers,
};
use super::fs::{DirectoryTreeEntry, FilesystemCopier, FilesystemHierarchy, PathRef};
use super::{FileStructureError, WriteError};

use maybe_async::maybe_async;

pub use super::progress_info::*;

struct DirentTableVec<'a> {
    dirent_tables: Vec<(PathRef<'a>, AvlDirectoryEntryTableWriter<'a>)>,
    count: usize,
}

fn create_dirent_tables<'a>(
    dirtree: &'a [DirectoryTreeEntry],
) -> Result<DirentTableVec<'a>, FileStructureError> {
    let mut dirent_tables: Vec<(PathRef<'_>, AvlDirectoryEntryTableWriter<'a>)> =
        Vec::with_capacity(dirtree.len());
    let mut dtab_size_map: Vec<u32> = vec![0u32; dirtree.len()];
    let mut count = 0;

    for (dir_idx, entry) in dirtree.iter().enumerate().rev() {
        let path = &entry.dir;
        let dir_entries = &entry.listing;

        let mut dirtab = AvlDirectoryEntryTableBuilder::default();
        dirtab.reserve(dir_entries.len());

        for (entry, dir_index) in dir_entries {
            let file_name = entry.name.as_str();
            count += 1;

            match entry.file_type {
                fs::FileType::Directory => {
                    debug_assert_eq!(
                        dirtree[*dir_index].dir.as_path_ref(),
                        path.as_path_ref().join(file_name),
                    );

                    let dir_size = dtab_size_map[*dir_index];
                    debug_assert_ne!(dir_size, 0);
                    dirtab.add_dir(file_name, dir_size, *dir_index)?;
                }
                fs::FileType::File => {
                    let file_size = entry
                        .len
                        .try_into()
                        .map_err(|_| FileStructureError::FileTooLarge)?;
                    dirtab.add_file(file_name, file_size)?;
                }
            }
        }

        // Store index of directory in dirent_tables in the dtab_size_map,
        // then pass it through the dirtab writer so it can be used as to look-up
        // sectors in the forward pass.
        let dtw = dirtab.build()?;
        let path = PathRef::from(path);

        dtab_size_map[dir_idx] = dtw.dirtab_size();
        dirent_tables.push((path, dtw));
    }

    Ok(DirentTableVec {
        dirent_tables,
        count,
    })
}

type GenericWriteError<BDW, FS> = WriteError<
    <BDW as BlockDeviceWrite>::WriteError,
    <FS as FilesystemHierarchy>::Error,
    <FS as FilesystemCopier<BDW>>::Error,
>;

pub struct XDVDFSImageWriter<
    'a,
    BDW: BlockDeviceWrite + ?Sized,
    FS: FilesystemHierarchy + FilesystemCopier<BDW> + ?Sized,
    PV: ProgressVisitor,
> {
    pub(super) fs: &'a mut FS,
    pub(super) image: &'a mut BDW,
    pub(super) progress_visitor: PV,
}

impl<'a, BDW, FS, CB> XDVDFSImageWriter<'a, BDW, FS, CB>
where
    BDW: BlockDeviceWrite + ?Sized,
    FS: FilesystemHierarchy + FilesystemCopier<BDW> + ?Sized,
    CB: ProgressVisitor,
{
    #[maybe_async]
    async fn write_volume_descriptor(
        &mut self,
        root_dirtab_size: u32,
        root_dirtab_sector: u32,
    ) -> Result<(), GenericWriteError<BDW, FS>> {
        // FIXME: Set timestamp
        let root_table = layout::DirectoryEntryTable::new(root_dirtab_size, root_dirtab_sector);
        let volume_info = layout::VolumeDescriptor::new(root_table);
        let volume_info = volume_info
            .serialize()
            .map_err(|_| FileStructureError::SerializationError)?;
        self.image
            .write(32 * layout::SECTOR_SIZE as u64, &volume_info)
            .await
            .map_err(WriteError::BlockDeviceError)?;
        Ok(())
    }

    #[maybe_async]
    async fn apply_image_alignment_padding(&mut self) -> Result<(), GenericWriteError<BDW, FS>> {
        let len = self
            .image
            .len()
            .await
            .map_err(WriteError::BlockDeviceError)?;

        let aligned = len.next_multiple_of(32 * layout::SECTOR_SIZE as u64);
        let padding = aligned - len;
        let padding: usize = padding.try_into().expect("padding < 32 * SECTOR_SIZE");

        if padding > 0 {
            let padding = vec![0x00; padding];
            self.image
                .write(len, &padding)
                .await
                .map_err(WriteError::BlockDeviceError)?;
        }

        Ok(())
    }

    #[maybe_async]
    async fn create_image(&mut self) -> Result<(), GenericWriteError<BDW, FS>> {
        // The size of a directory entry depends on the size of
        // directory entries inside it (including directory tables).
        // We need to compute the size of all dirent tables before
        // writing the image. As such, we iterate over a directory tree
        // in reverse order, such that dirents for leaf directories
        // are created before parents. Then, the other dirents can set their size
        // by tabulation.

        let mut dirtree_count_cb = |entry_count: usize| {
            self.progress_visitor.directory_discovered(entry_count);
        };
        let dirtree = fs::dir_tree(self.fs, &mut dirtree_count_cb)
            .await
            .map_err(WriteError::FilesystemHierarchyError)?;
        let DirentTableVec {
            dirent_tables,
            count: dirent_count,
        } = create_dirent_tables(&dirtree)?;

        self.progress_visitor
            .entry_counts(dirent_count, dirtree.len());

        // Now we can forward iterate through the dirtabs and allocate on-disk regions
        let mut sector_allocator = sector::SectorAllocator::default();
        let mut dir_sectors: Vec<u64> = vec![0u64; dirent_tables.len()];

        let root_dirtab = dirent_tables
            .last()
            .expect("should always have one dirent at minimum (root)");
        let root_dirtab_size = root_dirtab.1.dirtab_size();
        let root_sector = sector_allocator.allocate_contiguous(root_dirtab_size as u64);
        dir_sectors[0] = root_sector as u64;

        let mut dtw_buffers = DirtabWriterBuffers::default();

        for (dir_idx, (path, mut dirtab)) in dirent_tables.into_iter().rev().enumerate() {
            let dirtab_sector = dir_sectors[dir_idx];
            self.progress_visitor.directory_added(path, dirtab_sector);

            dirtab.disk_repr(&mut sector_allocator, &mut dtw_buffers)?;

            self.image
                .write(
                    dirtab_sector * layout::SECTOR_SIZE as u64,
                    &dtw_buffers.dirtab_bytes,
                )
                .await
                .map_err(WriteError::BlockDeviceError)?;

            for entry in dirtab.iter() {
                let file_path = path.join(entry.name);
                self.progress_visitor.file_added(file_path, entry.sector);

                if entry.is_dir {
                    dir_sectors[entry.idx] = entry.sector;
                    continue;
                }

                self.fs
                    .copy_file_in(
                        file_path,
                        self.image,
                        0,
                        entry.sector * layout::SECTOR_SIZE as u64,
                        entry.size,
                    )
                    .await
                    .map_err(WriteError::FilesystemCopierError)?;
            }
        }

        self.progress_visitor.finished_copying_image_data();

        self.write_volume_descriptor(root_dirtab_size, root_sector)
            .await?;
        self.apply_image_alignment_padding().await?;

        self.progress_visitor.finished();
        Ok(())
    }
}

#[maybe_async]
pub async fn create_xdvdfs_image<
    BDW: BlockDeviceWrite + ?Sized,
    FS: FilesystemHierarchy + FilesystemCopier<BDW> + ?Sized,
>(
    fs: &mut FS,
    image: &mut BDW,
    progress_visitor: impl ProgressVisitor,
) -> Result<(), GenericWriteError<BDW, FS>> {
    let mut img_writer = XDVDFSImageWriter {
        fs,
        image,
        progress_visitor,
    };

    img_writer.create_image().await
}

#[cfg(test)]
mod test {
    use alloc::string::ToString;

    use alloc::vec;
    use alloc::vec::Vec;

    use crate::blockdev::NullBlockDevice;
    use crate::layout;
    use crate::write::dirtab::FileListingEntry;
    use crate::write::fs::{
        FileEntry, MemoryFilesystem, PathRef, PathVec, SectorLinearBlockDevice,
        SectorLinearBlockFilesystem, SectorLinearBlockSectorContents,
    };
    use crate::write::img::{NoOpProgressVisitor, OwnedProgressInfo};

    use super::fs::DirectoryTreeEntry;
    use super::{create_dirent_tables, create_xdvdfs_image, ProgressInfo};

    #[test]
    fn test_create_dirent_tables_empty_root() {
        let dirtree = &[DirectoryTreeEntry {
            dir: "".into(),
            listing: Vec::new(),
        }];

        let dirtabs = create_dirent_tables(dirtree).expect("Dirtab should be valid");
        assert_eq!(dirtabs.count, 0);
        assert_eq!(dirtabs.dirent_tables.len(), 1);
    }

    #[test]
    fn test_create_dirent_tables_root_file() {
        let dirtree = &[DirectoryTreeEntry {
            dir: "".into(),
            listing: vec![(
                FileEntry {
                    name: "abc".to_string(),
                    file_type: crate::write::fs::FileType::File,
                    len: 10,
                },
                0,
            )],
        }];

        let dirtabs = create_dirent_tables(dirtree).expect("Dirtab should be valid");
        assert_eq!(dirtabs.count, 1);
        assert_eq!(dirtabs.dirent_tables.len(), 1);
    }

    #[test]
    fn test_create_dirent_tables_root_directory() {
        let dirtree = &[
            DirectoryTreeEntry {
                dir: "".into(),
                listing: vec![(
                    FileEntry {
                        name: "abc".to_string(),
                        file_type: crate::write::fs::FileType::Directory,
                        len: 0,
                    },
                    1,
                )],
            },
            DirectoryTreeEntry {
                dir: "/abc".into(),
                listing: Vec::new(),
            },
        ];

        let dirtabs = create_dirent_tables(dirtree).expect("Dirtab should be valid");
        assert_eq!(dirtabs.count, 1);
        assert_eq!(dirtabs.dirent_tables.len(), 2);
    }

    #[test]
    fn test_create_dirent_tables_nested_dirs() {
        let dirtree = &[
            DirectoryTreeEntry {
                dir: "".into(),
                listing: vec![(
                    FileEntry {
                        name: "abc".to_string(),
                        file_type: crate::write::fs::FileType::Directory,
                        len: 0,
                    },
                    1,
                )],
            },
            DirectoryTreeEntry {
                dir: "/abc".into(),
                listing: vec![(
                    FileEntry {
                        name: "def".to_string(),
                        file_type: crate::write::fs::FileType::File,
                        len: 5,
                    },
                    0,
                )],
            },
        ];

        let dirtabs = create_dirent_tables(dirtree).expect("Dirtab should be valid");
        assert_eq!(dirtabs.count, 2);
        assert_eq!(dirtabs.dirent_tables.len(), 2);
    }

    #[test]
    fn test_create_dirent_tables_entries_added() {
        let dirtree = &[
            DirectoryTreeEntry {
                dir: "".into(),
                listing: vec![(
                    FileEntry {
                        name: "abc".to_string(),
                        file_type: crate::write::fs::FileType::Directory,
                        len: 0,
                    },
                    1,
                )],
            },
            DirectoryTreeEntry {
                dir: "/abc".into(),
                listing: vec![(
                    FileEntry {
                        name: "def".to_string(),
                        file_type: crate::write::fs::FileType::File,
                        len: 5,
                    },
                    0,
                )],
            },
        ];

        let dirtabs = create_dirent_tables(dirtree).expect("Dirtab should be valid");
        assert_eq!(dirtabs.count, 2);
        assert_eq!(dirtabs.dirent_tables.len(), 2);

        let root = dirtabs.dirent_tables.last().unwrap();
        assert_eq!(root.0, PathVec::default());
        let listing: Vec<_> = root.1.iter().collect();
        assert_eq!(
            listing,
            &[FileListingEntry {
                name: "abc",
                sector: 0,
                size: 2048,
                is_dir: true,
                idx: 1,
            }],
        );

        let abc = dirtabs.dirent_tables.first().unwrap();
        assert_eq!(abc.0, PathVec::from("abc"));
        let listing: Vec<_> = abc.1.iter().collect();
        assert_eq!(
            listing,
            &[FileListingEntry {
                name: "def",
                sector: 0,
                size: 5,
                is_dir: false,
                idx: 0,
            },],
        );
    }

    #[test]
    fn test_create_xdvdfs_image_progress_callback() {
        let mut memfs = MemoryFilesystem::default();
        memfs.mkdir("/a");
        memfs.touch("/a/b");
        memfs.mkdir("/b");

        let mut nulldev = NullBlockDevice::default();

        let mut progress_list = Vec::new();

        let res = futures::executor::block_on(create_xdvdfs_image(
            &mut memfs,
            &mut nulldev,
            |pi: ProgressInfo<'_>| progress_list.push(pi.to_owned()),
        ));
        assert!(res.is_ok());

        assert_eq!(
            progress_list,
            &[
                OwnedProgressInfo::DiscoveredDirectory(2),
                OwnedProgressInfo::DiscoveredDirectory(1),
                OwnedProgressInfo::DiscoveredDirectory(0),
                OwnedProgressInfo::FileCount(3),
                OwnedProgressInfo::DirCount(3),
                OwnedProgressInfo::DirAdded(PathRef::from("/").into(), 33),
                OwnedProgressInfo::FileAdded(PathRef::from("/a").into(), 34),
                OwnedProgressInfo::FileAdded(PathRef::from("/b").into(), 35),
                OwnedProgressInfo::DirAdded(PathRef::from("/a").into(), 34),
                OwnedProgressInfo::FileAdded(PathRef::from("/a/b").into(), 36),
                OwnedProgressInfo::DirAdded(PathRef::from("/b").into(), 35),
                OwnedProgressInfo::FinishedCopyingImageData,
                OwnedProgressInfo::FinishedPacking,
            ]
        );
    }

    #[test]
    fn test_create_xdvdfs_image_32_sector_padding() {
        let mut memfs = MemoryFilesystem::default();
        let mut nulldev = NullBlockDevice::default();

        let res = futures::executor::block_on(create_xdvdfs_image(
            &mut memfs,
            &mut nulldev,
            NoOpProgressVisitor,
        ));
        assert!(res.is_ok());

        // Volume info at sector 32
        // Root dirent (empty) at sector 33
        // Padding up to 64
        assert_eq!(nulldev.len_blocking(), 64 * layout::SECTOR_SIZE as u64);
    }

    #[test]
    fn test_create_xdvdfs_image_32_sector_padding_not_applied() {
        let mut memfs = MemoryFilesystem::default();
        for i in 0..30 {
            // Volume info at sector 32
            // Root at sector 33 and 34
            // Need 30 more sectors to reach 64
            // (next multiple of 32-sector alignment)
            memfs.mkdir(std::format!("/{i}").as_str());
        }

        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        let res = futures::executor::block_on(create_xdvdfs_image(
            &mut slbfs,
            &mut slbd,
            NoOpProgressVisitor,
        ));
        assert!(res.is_ok());

        assert_eq!(slbd.num_sectors(), 64);

        // An empty sector is 0xff filled,
        // whereas padding is 0x00 filled
        let empty_dirent_sector = alloc::vec![0xff; 2048];
        assert_eq!(
            slbd.get(63),
            SectorLinearBlockSectorContents::RawData(&empty_dirent_sector)
        );
    }

    #[test]
    fn test_create_xdvdfs_image_directories_and_files_copied_in() {
        let mut memfs = MemoryFilesystem::default();
        memfs.mkdir("/a");
        memfs.touch("/a/b");
        memfs.mkdir("/b");

        let mut slbd = SectorLinearBlockDevice::default();
        let mut slbfs = SectorLinearBlockFilesystem::new(memfs);

        let res = futures::executor::block_on(create_xdvdfs_image(
            &mut slbfs,
            &mut slbd,
            NoOpProgressVisitor,
        ));
        assert!(res.is_ok());

        // Check directory tables are written
        // Volume descriptor
        assert!(matches!(
            slbd.get(32),
            SectorLinearBlockSectorContents::RawData(_)
        ));
        // "/"
        assert!(matches!(
            slbd.get(33),
            SectorLinearBlockSectorContents::RawData(_)
        ));
        // "/a"
        assert!(matches!(
            slbd.get(34),
            SectorLinearBlockSectorContents::RawData(_)
        ));
        // "/b"
        assert!(matches!(
            slbd.get(35),
            SectorLinearBlockSectorContents::RawData(_)
        ));
        // "/a/b"
        assert_eq!(
            slbd.get(36),
            SectorLinearBlockSectorContents::File("/a/b".into())
        );
    }
}
