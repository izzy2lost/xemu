use fs::{FilesystemCopier, FilesystemHierarchy};

use crate::blockdev::BlockDeviceWrite;
use crate::layout::NameEncodingError;

use thiserror::Error;

mod avl;
pub mod dirtab;
pub mod fs;
pub mod img;
pub mod sector;

mod progress_info;

/// Contains variants of WriteError that are not specific to
/// the block device or filesystem. This allows us to pass errors
/// around the write module without having to carry around the generics.
#[derive(Error, Debug, PartialEq, Eq)]
pub enum FileStructureError {
    #[error("serialization failed")]
    SerializationError,
    #[error(transparent)]
    FileNameError(#[from] NameEncodingError),
    #[error("file name already exists")]
    DuplicateFileName,
    #[error("too many entries in a single directory")]
    TooManyDirectoryEntries,
    #[error("file is too large")]
    FileTooLarge,
}

/// Error states that can result during image creation.
/// File structure errors are specific to the contents of the filesystem,
/// whereas the other variants originate from the filesystem and block
/// device abstractions.
#[derive(Error, Debug, PartialEq, Eq)]
pub enum WriteError<BlockDevError, FSHierarchyError, FSCopierError> {
    #[error("failed to write to block device: {0}")]
    BlockDeviceError(#[source] BlockDevError),
    #[error("failed to read source filesystem hierarchy: {0}")]
    FilesystemHierarchyError(#[source] FSHierarchyError),
    #[error("failed to copy from filesystem into block device: {0}")]
    FilesystemCopierError(#[source] FSCopierError),
    #[error("failed to create xdvdfs filesystem: {0}")]
    InvalidFileStructureError(#[from] FileStructureError),
}

pub type GenericWriteError<BDW, FS> = WriteError<
    <BDW as BlockDeviceWrite>::WriteError,
    <FS as FilesystemHierarchy>::Error,
    <FS as FilesystemCopier<BDW>>::Error,
>;
