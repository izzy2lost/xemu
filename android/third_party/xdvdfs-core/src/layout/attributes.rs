use core::fmt::Display;

use proc_bitfield::bitfield;
use serde::{Deserialize, Serialize};

bitfield!(
#[repr(C)]
#[derive(Deserialize, Serialize, Copy, Clone, Eq, PartialEq, Hash)]
pub struct DirentAttributes(pub u8): Debug {
    pub attrs: u8 @ ..,

    pub read_only: bool @ 0,
    pub hidden: bool @ 1,
    pub system: bool @ 2,
    pub directory: bool @ 4,
    pub archive: bool @ 5,
    pub normal: bool @ 7,
}
);

fn print_leading_space(f: &mut core::fmt::Formatter<'_>, has_prev: &mut bool) -> core::fmt::Result {
    if *has_prev {
        f.write_str(" ")?;
    }

    *has_prev = true;
    Ok(())
}

impl Display for DirentAttributes {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let mut has_prev = false;

        if self.directory() {
            print_leading_space(f, &mut has_prev)?;
            f.write_str("Directory")?;
        }

        if self.read_only() {
            print_leading_space(f, &mut has_prev)?;
            f.write_str("Read-Only")?;
        }

        if self.hidden() {
            print_leading_space(f, &mut has_prev)?;
            f.write_str("Hidden")?;
        }

        if self.system() {
            print_leading_space(f, &mut has_prev)?;
            f.write_str("System")?;
        }

        if self.archive() {
            print_leading_space(f, &mut has_prev)?;
            f.write_str("Archive")?;
        }

        if self.normal() {
            print_leading_space(f, &mut has_prev)?;
            f.write_str("Normal")?;
        }

        Ok(())
    }
}

#[cfg(test)]
mod test {
    use super::DirentAttributes;

    #[test]
    fn test_layout_attributes_display() {
        use alloc::string::ToString;

        assert_eq!(DirentAttributes(1 << 0).to_string(), "Read-Only");
        assert_eq!(DirentAttributes(1 << 1).to_string(), "Hidden");
        assert_eq!(DirentAttributes(1 << 2).to_string(), "System");
        assert_eq!(DirentAttributes(1 << 4).to_string(), "Directory");
        assert_eq!(DirentAttributes(1 << 5).to_string(), "Archive");
        assert_eq!(DirentAttributes(1 << 7).to_string(), "Normal");
        assert_eq!(
            DirentAttributes(0xff).to_string(),
            "Directory Read-Only Hidden System Archive Normal",
        );
    }
}
