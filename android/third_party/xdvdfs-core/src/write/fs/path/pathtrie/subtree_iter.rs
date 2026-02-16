use alloc::boxed::Box;

use super::subtree::PPTSubtree;
use super::PathPrefixTree;

pub enum PPTSubtreeIter<'a, T> {
    Empty,
    SingleChar(char, Option<&'a PathPrefixTree<T>>),
    CharArray {
        index: u8,
        children: alloc::collections::vec_deque::Iter<'a, Option<Box<PathPrefixTree<T>>>>,
    },
}

impl<T> PPTSubtree<T> {
    pub fn iter(&self) -> PPTSubtreeIter<'_, T> {
        match self {
            Self::Empty => PPTSubtreeIter::Empty,
            Self::SingleChar(ch, subtree) => {
                PPTSubtreeIter::SingleChar(*ch, subtree.as_ref().map(|x| x.as_ref()))
            }
            Self::CharArray {
                start_char,
                children,
            } => PPTSubtreeIter::CharArray {
                index: *start_char as u8,
                children: children.iter(),
            },
        }
    }
}

impl<'a, T> Iterator for PPTSubtreeIter<'a, T> {
    type Item = (char, Option<&'a PathPrefixTree<T>>);

    fn next(&mut self) -> Option<Self::Item> {
        match self {
            Self::Empty => None,
            Self::SingleChar(ch, subtree) => {
                let ret = (*ch, *subtree);
                *self = Self::Empty;
                Some(ret)
            }
            Self::CharArray { index, children } => {
                let ch = *index as char;
                let Some(subtree) = children.next() else {
                    *self = Self::Empty;
                    return None;
                };

                let subtree = subtree.as_ref().map(|x| x.as_ref());
                *index += 1;
                Some((ch, subtree))
            }
        }
    }
}

#[cfg(test)]
mod test {
    use alloc::boxed::Box;
    use alloc::collections::VecDeque;
    use alloc::vec::Vec;

    use crate::write::fs::{path::pathtrie::subtree::PPTSubtree, PathPrefixTree};

    #[test]
    fn test_trie_subtree_iter_empty() {
        let subtree = PPTSubtree::<()>::Empty;
        let values: Vec<_> = subtree.iter().collect();
        assert_eq!(values, &[]);
    }

    #[test]
    fn test_trie_subtree_iter_single_char() {
        let subtree = PPTSubtree::<()>::SingleChar('a', Some(Box::new(PathPrefixTree::default())));
        let values: Vec<_> = subtree.iter().collect();
        assert_eq!(values, &[('a', Some(&PathPrefixTree::default())),]);
    }

    #[test]
    fn test_trie_subtree_iter_char_array() {
        let mut children = VecDeque::new();
        children.push_back(Some(Box::new(PathPrefixTree::default())));
        children.resize(3, None);
        children.push_back(Some(Box::new(PathPrefixTree::default())));
        let subtree = PPTSubtree::<()>::CharArray {
            start_char: 'a',
            children,
        };
        let values: Vec<_> = subtree.iter().collect();
        assert_eq!(
            values,
            &[
                ('a', Some(&PathPrefixTree::default())),
                ('b', None),
                ('c', None),
                ('d', Some(&PathPrefixTree::default())),
            ]
        );
    }
}
