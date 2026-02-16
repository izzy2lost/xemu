use thiserror::Error;

use crate::layout::{NameDeserializationError, OutOfBounds};

mod dirent_node;
mod dirent_table;

mod disk_data;
pub use disk_data::*;

mod scan_iter;
pub use scan_iter::*;

mod volume;
pub use volume::*;

/// Error states that occur when reading a directory
/// entry from disk.
#[non_exhaustive]
#[derive(Error, Debug, Copy, Clone, Eq, PartialEq)]
pub enum DirectoryEntryReadError<E> {
    #[error("io error")]
    IOError(#[source] E),
    #[error("deserialization failed")]
    DeserializationFailed,
}

/// Error states that occur when walking a directory
/// entry table in binary-tree preorder.
#[non_exhaustive]
#[derive(Error, Debug, Copy, Clone, Eq, PartialEq)]
pub enum DirectoryTableWalkError<E> {
    #[error("failed to read directory entry")]
    DirectoryEntryReadFailed(#[from] DirectoryEntryReadError<E>),
    #[error("offset out of bounds")]
    SizeOutOfBounds(#[from] OutOfBounds),
    #[error("failed to decode filename")]
    StringEncodingError(#[from] NameDeserializationError),
}

/// Error states that occur when looking up an
/// entry in a directory entry table by name.
#[non_exhaustive]
#[derive(Error, Debug, Copy, Clone, Eq, PartialEq)]
pub enum DirectoryTableLookupError<E> {
    #[error("io error")]
    IOError(#[source] E),
    #[error("offset out of bounds")]
    SizeOutOfBounds(#[from] OutOfBounds),
    #[error("failed to ready directory entry")]
    DirectoryEntryReadFailed(#[from] DirectoryEntryReadError<E>),
    #[error("serialization failed")]
    SerializationFailed,
    #[error("directory is empty")]
    DirectoryEmpty,
    #[error("entry does not exist")]
    DoesNotExist,
    #[error("failed to decode filename")]
    StringEncodingError(#[from] NameDeserializationError),
    #[error("path exists, but dirent does not (likely root)")]
    NoDirent,
    #[error("expected directory, found file")]
    IsNotDirectory,
}
