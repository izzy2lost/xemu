use super::PathRef;

impl Ord for PathRef<'_> {
    fn cmp(&self, other: &Self) -> core::cmp::Ordering {
        use core::cmp::Ordering;

        // Handle joined paths recursively to avoid allocating
        // in the iterator for each comparison
        if let (Self::Join(p1_rest, p1_tail), Self::Join(p2_rest, p2_tail)) = (self, other) {
            return match p1_rest.cmp(p2_rest) {
                Ordering::Equal => p1_tail.cmp(p2_tail),
                cmp => cmp,
            };
        }

        let mut p1_iter = self.iter();
        let mut p2_iter = other.iter();
        loop {
            match (p1_iter.next(), p2_iter.next()) {
                (Some(component_1), Some(component_2)) => match component_1.cmp(component_2) {
                    Ordering::Equal => continue,
                    cmp => break cmp,
                },
                (None, None) => break Ordering::Equal,
                (None, _) => break Ordering::Less,
                (_, None) => break Ordering::Greater,
            }
        }
    }
}

impl<'a, 'b: 'a> PartialOrd<PathRef<'b>> for PathRef<'a> {
    fn partial_cmp(&self, other: &PathRef<'a>) -> Option<core::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

#[cfg(test)]
mod test {
    use crate::write::fs::PathRef;

    #[test]
    fn test_pathref_cmp_non_joined_equal() {
        let path1: PathRef = "/hello/world".into();
        let path2: PathRef = "/hello/world".into();

        assert_eq!(path1.cmp(&path2), core::cmp::Ordering::Equal);
    }

    #[test]
    fn test_pathref_cmp_non_joined_lt() {
        let path1: PathRef = "/hello/world".into();
        let path2: PathRef = "/hello/worlds".into();

        assert!(path1 < path2);
    }

    #[test]
    fn test_pathref_cmp_non_joined_lt_component() {
        let path1: PathRef = "/hello".into();
        let path2: PathRef = "/hello/world".into();

        assert!(path1 < path2);
    }

    #[test]
    fn test_pathref_cmp_non_joined_gt_component() {
        let path1: PathRef = "/hello/world".into();
        let path2: PathRef = "/hello".into();

        assert!(path1 > path2);
    }

    #[test]
    fn test_pathref_cmp_joined_eq() {
        let hello = PathRef::from("hello");
        let path1 = PathRef::Join(&hello, "world");
        let path2 = PathRef::Join(&hello, "world");

        assert_eq!(path1.cmp(&path2), core::cmp::Ordering::Equal);
    }

    #[test]
    fn test_pathref_cmp_joined_lt_tail() {
        let hello = PathRef::from("hello");
        let path1 = PathRef::Join(&hello, "world");
        let path2 = PathRef::Join(&hello, "worlds");

        assert!(path1 < path2);
    }

    #[test]
    fn test_pathref_cmp_joined_gt_base() {
        let hello = PathRef::from("hello");
        let path1 = PathRef::Join(&hello, "world");
        let greetings = PathRef::from("greetings");
        let path2 = PathRef::Join(&greetings, "world");

        assert!(path1 > path2);
    }
}
