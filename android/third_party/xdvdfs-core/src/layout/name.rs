use thiserror::Error;

pub fn cmp_ignore_case_utf8(a: &str, b: &str) -> core::cmp::Ordering {
    use core::cmp::Ordering;

    let mut a_chars = a.chars().map(|c| c.to_ascii_uppercase());
    let mut b_chars = b.chars().map(|c| c.to_ascii_uppercase());

    loop {
        let a_next = a_chars.next();
        let b_next = b_chars.next();
        if a_next.is_none() && b_next.is_none() {
            break Ordering::Equal;
        }

        let a = match a_next {
            Some(a) => a,
            None => break Ordering::Less,
        };

        let b = match b_next {
            Some(b) => b,
            None => break Ordering::Greater,
        };

        match a.cmp(&b) {
            Ordering::Equal => continue,
            cmp => break cmp,
        }
    }
}

#[derive(Error, Copy, Clone, Debug, Eq, PartialEq)]
pub enum NameEncodingError {
    #[error("file name is too long")]
    NameTooLong,
    #[error("cannot encode string into utf-8")]
    StringEncodingError,
}

// When building a directory entry table, comparisons between entries
// is done by the name string with upper-case characters. When serializing,
// we no longer need to perform comparisons, but do need the serialized
// name, so we reuse the space.
#[derive(Copy, Clone, Debug, Hash)]
enum DirentNameStored {
    WithUpperCase([u8; 255]),
    WithEncoding([u8; 255], u8),
}

#[derive(Copy, Clone, Debug, Hash)]
pub struct DirentName<'alloc> {
    name: &'alloc str,
    name_inner: DirentNameStored,
}

impl<'alloc> DirentName<'alloc> {
    pub fn new(name: &'alloc str) -> Self {
        // Truncate for the purpose of comparison,
        // but keep full name for encoding.
        let name_len = core::cmp::min(name.len(), 255);

        // UTF-8 alignment check is desirable here
        #[allow(clippy::sliced_string_as_bytes)]
        let name_bytes = name[..name_len].as_bytes();

        let mut name_inner = [0u8; 255];
        name_inner[..name_len].copy_from_slice(name_bytes);

        // SAFETY: name_bytes was just copied from a UTF-8 str
        let name_inner_str =
            unsafe { core::str::from_utf8_unchecked_mut(&mut name_inner[..name_len]) };
        name_inner_str.make_ascii_uppercase();

        let name_inner = DirentNameStored::WithUpperCase(name_inner);
        Self { name, name_inner }
    }

    pub fn get_name(&self) -> &str {
        self.name
    }

    pub fn set_encode_name(&mut self) -> Result<u8, NameEncodingError> {
        use encoding_rs::EncoderResult;

        if let DirentNameStored::WithEncoding(_, size) = self.name_inner {
            return Ok(size);
        }

        let mut buffer = [0u8; 255];
        let mut encoder = encoding_rs::WINDOWS_1252.new_encoder();

        let (result, bytes_read, bytes_written) =
            encoder.encode_from_utf8_without_replacement(self.name, &mut buffer, true);
        match result {
            EncoderResult::InputEmpty => {}
            EncoderResult::OutputFull => return Err(NameEncodingError::NameTooLong),
            EncoderResult::Unmappable(_) => return Err(NameEncodingError::StringEncodingError),
        }

        assert!(bytes_written <= 255);
        self.name_inner = DirentNameStored::WithEncoding(buffer, bytes_written as u8);

        assert_eq!(bytes_read, self.name.len());
        Ok(bytes_written as u8)
    }

    pub fn get_encoded_name(&self) -> &[u8] {
        match &self.name_inner {
            DirentNameStored::WithEncoding(buf, len) => &buf[..(*len as usize)],
            _ => unreachable!("Must call set_encoded_name before get!"),
        }
    }
}

impl PartialEq for DirentName<'_> {
    fn eq(&self, other: &Self) -> bool {
        self.cmp(other) == core::cmp::Ordering::Equal
    }
}

impl Eq for DirentName<'_> {}

impl PartialOrd for DirentName<'_> {
    fn partial_cmp(&self, other: &Self) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

impl Ord for DirentName<'_> {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        // If comparing serialized names, fall back to character-by-character
        // comparison.
        let DirentNameStored::WithUpperCase(self_name) = &self.name_inner else {
            // TODO: Annotate with `cold_path()` once it is stabilized
            return cmp_ignore_case_utf8(self.name, other.name);
        };

        let DirentNameStored::WithUpperCase(other_name) = &other.name_inner else {
            return cmp_ignore_case_utf8(self.name, other.name);
        };

        // SAFETY: WithUpperCase is constructed with a UTF-8 str
        // FIXME: Slice names by actual length
        let self_name = unsafe { core::str::from_utf8_unchecked(self_name) };
        let other_name = unsafe { core::str::from_utf8_unchecked(other_name) };

        self_name.cmp(other_name)
    }
}

#[cfg(test)]
fn name_comparator_wrapper(a: &str, b: &str) -> core::cmp::Ordering {
    let a = DirentName::new(a);
    let b = DirentName::new(b);
    a.cmp(&b)
}

macro_rules! case_cmp_test {
    ($name:ident, $cmp:expr) => {
        #[cfg(test)]
        mod $name {
            #[test]
            fn test_str_ignore_case_alpha() {
                let mut strings = ["asdf", "GHJK", "bsdf", "AAAA"];
                strings.sort_by($cmp);

                assert_eq!(strings, ["AAAA", "asdf", "bsdf", "GHJK"]);
            }

            #[test]
            fn test_str_ignore_case_underscore_gt_alpha() {
                let mut strings = ["a_b", "abb"];
                strings.sort_by($cmp);

                assert_eq!(strings, ["abb", "a_b"]);
            }

            #[test]
            fn test_str_lt_gt_len() {
                let s1 = "abc";
                let s2 = "abcd";

                let mut lt = [s1, s2];
                lt.sort_by($cmp);
                assert_eq!(lt, [s1, s2]);

                let mut gt = [s2, s1];
                gt.sort_by($cmp);
                assert_eq!(gt, [s1, s2]);
            }

            #[test]
            fn test_str_ignore_case_ordering() {
                let mut strings = [
                    "FILE.ext",
                    "FILElower.xyz",
                    "FILElower-hyphen.xyz",
                    "FILElowerlonger.asd",
                    "FILE-hyphen.ext",
                    "abc",
                ];
                strings.sort_by($cmp);

                assert_eq!(
                    strings,
                    [
                        "abc",
                        "FILE-hyphen.ext",
                        "FILE.ext",
                        "FILElower-hyphen.xyz",
                        "FILElower.xyz",
                        "FILElowerlonger.asd",
                    ]
                );
            }
        }
    };
}

case_cmp_test!(test_layout_cmp_fn, |a, b| {
    crate::layout::cmp_ignore_case_utf8(a, b)
});
case_cmp_test!(test_layout_dirent_name_cmp, |a, b| {
    crate::layout::name::name_comparator_wrapper(a, b)
});

#[cfg(test)]
mod test {
    use alloc::string::String;

    use crate::layout::NameEncodingError;

    use super::DirentName;

    #[test]
    fn test_layout_dirent_name_eq() {
        let n1 = DirentName::new("a");
        let n2 = DirentName::new("A");

        assert_eq!(n1, n2);
        assert_eq!(n1.partial_cmp(&n2), Some(core::cmp::Ordering::Equal));
    }

    #[test]
    fn test_layout_dirent_name_fallback_cmp() {
        let mut n1 = DirentName::new("abcd");
        let mut n2 = DirentName::new("ABCD");
        let mut n3 = DirentName::new("aBcD");

        assert_eq!(n1, n2);
        assert_eq!(n2, n3);
        n1.set_encode_name().expect("Encoding n1 should succeed");
        assert_eq!(n1, n2);
        assert_eq!(n2, n3);
        n3.set_encode_name().expect("Encoding n3 should succeed");
        assert_eq!(n1, n2);
        assert_eq!(n2, n3);
        n2.set_encode_name().expect("Encoding n2 should succeed");
        assert_eq!(n1, n2);
        assert_eq!(n2, n3);
    }

    #[test]
    fn test_layout_dirent_name_encode_max_len() {
        let mut s = String::with_capacity(255);
        for _ in 0..255 {
            s.push('a');
        }

        let mut name = DirentName::new(&s);
        let res = name.set_encode_name();
        assert_eq!(res, Ok(255));

        let res = name.set_encode_name();
        assert_eq!(res, Ok(255));
    }

    #[test]
    fn test_layout_dirent_name_encode_over_max_len() {
        let mut s = String::with_capacity(256);
        for i in 0..256 {
            let x = ('a' as u8) + (i % 26) as u8;
            s.push(x as char);
        }

        let mut name = DirentName::new(&s);
        let res = name.set_encode_name();
        assert_eq!(res, Err(NameEncodingError::NameTooLong));
    }

    #[test]
    fn test_layout_dirent_name_encode_invalid_char() {
        let mut name = DirentName::new("😀");
        let res = name.set_encode_name();
        assert_eq!(res, Err(NameEncodingError::StringEncodingError));
    }
}
