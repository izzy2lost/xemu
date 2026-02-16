#![no_std]

extern crate alloc;
#[cfg(feature = "std")]
extern crate std;

macro_rules! traceln {
    ($($x:expr),*) => {
        #[cfg(all(feature = "std", feature = "logging"))]
        log::trace!($($x),*);
    };
}

macro_rules! debugln {
    ($($x:expr),*) => {
        #[cfg(all(feature = "std", feature = "logging"))]
        log::debug!($($x),*);
    };
}

pub mod blockdev;
pub mod layout;

#[cfg(feature = "read")]
pub mod read;

#[cfg(feature = "write")]
pub mod write;

#[cfg(all(feature = "checksum", feature = "std"))]
pub mod checksum;
