use crate::write::fs::PathVec;

use super::PathRef;

impl<'a, 'b: 'a> PartialEq<PathRef<'b>> for PathRef<'a> {
    fn eq(&self, other: &PathRef<'b>) -> bool {
        // Avoid potential allocation if both paths are joined
        if let Self::Join(base, tail) = self {
            if let Self::Join(other_base, other_tail) = other {
                return tail == other_tail && base == other_base;
            }
        }

        let mut i1 = self.iter();
        let mut i2 = other.iter();

        loop {
            match (i1.next(), i2.next()) {
                (Some(c1), Some(c2)) if c1 == c2 => continue,
                (None, None) => break true,
                _ => break false,
            }
        }
    }
}

impl PartialEq<PathVec> for PathRef<'_> {
    fn eq(&self, other: &PathVec) -> bool {
        self.eq(&PathRef::from(other))
    }
}

impl PartialEq<&str> for PathRef<'_> {
    fn eq(&self, other: &&str) -> bool {
        self.eq(&PathRef::from(*other))
    }
}

impl PartialEq<&[&'_ str]> for PathRef<'_> {
    fn eq(&self, other: &&[&str]) -> bool {
        self.eq(&PathRef::from(*other))
    }
}

#[cfg(test)]
mod test {
    use crate::write::fs::PathRef;

    #[test]
    fn test_pathref_eq_equal() {
        let p1: PathRef = "/hello/world".into();
        let p2: PathRef = "/hello/world".into();

        assert_eq!(p1, p2);
    }

    #[test]
    fn test_pathref_eq_unequal_components() {
        let p1: PathRef = "/hello/world".into();
        let p2: PathRef = "/hello/universe".into();

        assert_ne!(p1, p2);
    }

    #[test]
    fn test_pathref_eq_unequal_lengths() {
        let p1: PathRef = "/hello/world".into();
        let p2: PathRef = "/hello".into();

        assert_ne!(p1, p2);
    }

    #[test]
    fn test_pathref_eq_both_joined() {
        let hello: PathRef = "/hello/".into();
        let p1 = hello.join("world");
        let p2 = hello.join("world");

        assert_eq!(p1, p2);
    }

    #[test]
    fn test_pathref_eq_one_joined() {
        let hello: PathRef = "/hello/".into();
        let p1 = hello.join("world");
        let p2: PathRef = "/hello/world".into();

        assert_eq!(p1, p2);
        assert_eq!(p2, p1);
    }

    #[test]
    fn test_pathref_eq_both_joined_depth() {
        let hello: PathRef = "/hello/".into();
        let p1 = hello.join("world");
        let p1 = p1.join("abc");
        let p2 = hello.join("world");
        let p2 = p2.join("abc");

        assert_eq!(p1, p2);
    }

    #[test]
    fn test_pathref_str_eq() {
        let hello: PathRef = "/hello/world".into();

        assert_eq!(hello, "/hello/world");
    }

    #[test]
    fn test_pathref_slice_eq() {
        let hello: PathRef = "/hello/world".into();
        assert_eq!(hello, ["hello", "world"].as_slice());
    }

    #[test]
    fn test_pathref_pathvec_eq() {
        let hello: PathRef = "/hello/world".into();
        let hello_pv: super::PathVec = "/hello/world".into();
        assert_eq!(hello, hello_pv);
    }
}
