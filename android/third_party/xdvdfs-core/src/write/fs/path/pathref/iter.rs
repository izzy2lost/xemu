use core::{
    iter::{Chain, Filter, FusedIterator},
    str::Split,
};

use crate::write::fs::PathVecIter;

use super::{joined::JoinedPathRefIter, PathRef};

fn empty_component_filter(a: &&str) -> bool {
    !a.is_empty()
}

type StrIterType<'a> = Filter<Split<'a, &'static str>, for<'b> fn(&'b &'a str) -> bool>;

pub enum BasePathRefIter<'a> {
    Str(StrIterType<'a>),
    Slice(core::slice::Iter<'a, &'a str>),
    PathVec(PathVecIter<'a>),
}

pub enum PathRefIter<'a> {
    Base(BasePathRefIter<'a>),

    // Split out single joins to avoid an allocation
    // If we join more than once (e.g. "/".into().join("a").join("b"))
    // then fall back to an allocating iterator.
    SingleJoin(Chain<BasePathRefIter<'a>, core::iter::Once<&'a str>>),
    JoinedIter(JoinedPathRefIter<'a>),
}

impl<'a> IntoIterator for &PathRef<'a> {
    type Item = &'a str;
    type IntoIter = PathRefIter<'a>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'a> PathRef<'a> {
    pub fn iter(&self) -> PathRefIter<'a> {
        match self {
            Self::Str(s) => PathRefIter::Base(BasePathRefIter::Str(
                s.split("/").filter(empty_component_filter),
            )),
            Self::Slice(s) => PathRefIter::Base(BasePathRefIter::Slice(s.iter())),
            Self::PathVec(pv) => PathRefIter::Base(BasePathRefIter::PathVec(pv.iter())),
            Self::Join(base, tail) => {
                let tail_iter = core::iter::once(*tail);
                match base.iter() {
                    PathRefIter::Base(base_iter) => {
                        PathRefIter::SingleJoin(base_iter.chain(tail_iter))
                    }
                    _ => PathRefIter::JoinedIter(JoinedPathRefIter::new(base, tail)),
                }
            }
        }
    }
}

impl<'a> Iterator for BasePathRefIter<'a> {
    type Item = &'a str;

    fn next(&mut self) -> Option<Self::Item> {
        match self {
            Self::Str(iter) => iter.next(),
            Self::Slice(iter) => iter.next().map(|s| &**s),
            Self::PathVec(iter) => iter.next(),
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        match self {
            Self::Str(iter) => iter.size_hint(),
            Self::Slice(iter) => iter.size_hint(),
            Self::PathVec(iter) => iter.size_hint(),
        }
    }
}

impl FusedIterator for BasePathRefIter<'_> {}

impl<'a> Iterator for PathRefIter<'a> {
    type Item = &'a str;

    fn next(&mut self) -> Option<Self::Item> {
        match self {
            Self::Base(iter) => iter.next(),
            Self::SingleJoin(iter) => iter.next(),
            Self::JoinedIter(iter) => iter.next(),
        }
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        match self {
            Self::Base(iter) => iter.size_hint(),
            Self::SingleJoin(iter) => iter.size_hint(),
            Self::JoinedIter(iter) => iter.size_hint(),
        }
    }
}

impl FusedIterator for PathRefIter<'_> {}

#[cfg(test)]
mod test {
    use alloc::vec::Vec;

    use crate::write::fs::path::PathRef;

    #[test]
    fn test_pathref_into_iterator() {
        let path: PathRef = "/abc/def".into();
        let components: Vec<_> = path.into_iter().collect();

        assert_eq!(components, &["abc", "def"]);
    }
}
