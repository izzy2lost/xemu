use core::ops::{Index, IndexMut};

use alloc::boxed::Box;
use alloc::collections::VecDeque;

use super::PathPrefixTree;

#[derive(Default, Clone, Debug, Eq, PartialEq)]
pub enum PPTSubtree<T> {
    #[default]
    Empty,
    SingleChar(char, Option<Box<PathPrefixTree<T>>>),
    CharArray {
        start_char: char,
        children: VecDeque<Option<Box<PathPrefixTree<T>>>>,
    },
}

impl<T> Index<char> for PPTSubtree<T> {
    type Output = Option<Box<PathPrefixTree<T>>>;

    fn index(&self, index: char) -> &Self::Output {
        match self {
            Self::SingleChar(ch, subtree) if *ch == index => subtree,
            Self::CharArray {
                start_char,
                children,
            } if index >= *start_char => {
                let index = index as u8;
                let start_char = *start_char as u8;
                children.get((index - start_char) as usize).unwrap_or(&None)
            }
            _ => &None,
        }
    }
}

impl<T> IndexMut<char> for PPTSubtree<T> {
    fn index_mut(&mut self, index: char) -> &mut Self::Output {
        // If the new index cannot be inserted, upgrade the internal representation
        match self {
            Self::Empty => *self = Self::SingleChar(index, None),
            Self::SingleChar(ch, subtree) if *ch != index => {
                let mut children = VecDeque::new();
                children.push_back(subtree.take());
                *self = Self::CharArray {
                    start_char: *ch,
                    children,
                };
            }
            _ => {}
        }

        // At this point, the index must be valid
        match self {
            Self::Empty => unreachable!("Empty should have upgraded to SingleChar"),
            Self::SingleChar(ch, subtree) if *ch == index => subtree,
            Self::SingleChar(_, _) => {
                unreachable!("Non-matching SingleChar should have upgraded to CharArray")
            }
            Self::CharArray {
                start_char,
                children,
            } => {
                if index == *start_char {
                    let idx = (index as u8) - (*start_char as u8);
                    &mut children[idx as usize]
                } else if index > *start_char {
                    // index is in the back of the deque, extend if needed
                    let idx = (index as u8) - (*start_char as u8);
                    let idx = idx as usize;
                    if idx >= children.len() {
                        children.resize_with(idx + 1, || None);
                    }

                    &mut children[idx]
                } else {
                    // index < *start_char
                    let prev_start_char = *start_char as u8;
                    *start_char = index;

                    // index is in the front of the deque, extend front if needed
                    // New index becomes the start_char
                    let index = index as u8;
                    for _ in index..prev_start_char {
                        children.push_front(None);
                    }

                    &mut children[0]
                }
            }
        }
    }
}

#[cfg(test)]
mod test {
    use alloc::boxed::Box;

    use crate::write::fs::PathPrefixTree;

    use super::PPTSubtree;

    #[test]
    fn test_ppt_subtree_single_record() {
        let mut subtree = PPTSubtree::<u32>::Empty;

        subtree['a'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 32);
            Some(ppt)
        };

        assert!(subtree['a'].is_some());
        assert_eq!(subtree['a'].as_ref().unwrap().get(""), Some(&32));
        assert!(subtree['b'].is_none());
    }

    #[test]
    fn test_ppt_subtree_single_record_update() {
        let mut subtree = PPTSubtree::<u32>::Empty;

        subtree['a'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 32);
            Some(ppt)
        };

        assert!(subtree['a'].is_some());
        assert_eq!(subtree['a'].as_ref().unwrap().get(""), Some(&32));

        subtree['a'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 40);
            Some(ppt)
        };

        assert!(subtree['a'].is_some());
        assert_eq!(subtree['a'].as_ref().unwrap().get(""), Some(&40));
    }

    #[test]
    fn test_ppt_subtree_multi_record_increasing() {
        let mut subtree = PPTSubtree::<u32>::Empty;

        subtree['a'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 32);
            Some(ppt)
        };

        assert!(subtree['a'].is_some());
        assert!(subtree['g'].is_none());

        subtree['g'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 40);
            Some(ppt)
        };

        assert_eq!(subtree['a'].as_ref().unwrap().get(""), Some(&32));

        assert!(subtree['g'].is_some());
        assert_eq!(subtree['g'].as_ref().unwrap().get(""), Some(&40));
    }

    #[test]
    fn test_ppt_subtree_multi_record_decreasing() {
        let mut subtree = PPTSubtree::<u32>::Empty;

        subtree['g'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 32);
            Some(ppt)
        };

        assert!(subtree['g'].is_some());
        assert!(subtree['c'].is_none());

        subtree['c'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 40);
            Some(ppt)
        };

        assert_eq!(subtree['g'].as_ref().unwrap().get(""), Some(&32));

        assert!(subtree['c'].is_some());
        assert_eq!(subtree['c'].as_ref().unwrap().get(""), Some(&40));
    }

    #[test]
    fn test_ppt_subtree_multi_record_update() {
        let mut subtree = PPTSubtree::<u32>::Empty;

        subtree['g'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 32);
            Some(ppt)
        };

        assert!(subtree['g'].is_some());
        assert!(subtree['c'].is_none());

        subtree['c'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 40);
            Some(ppt)
        };

        assert_eq!(subtree['g'].as_ref().unwrap().get(""), Some(&32));

        assert!(subtree['c'].is_some());
        assert_eq!(subtree['c'].as_ref().unwrap().get(""), Some(&40));

        subtree['g'] = None;
        assert!(subtree['g'].is_none());
        assert!(subtree['c'].is_some());
    }

    #[test]
    fn test_ppt_subtree_multi_record_update_lesser() {
        let mut subtree = PPTSubtree::<u32>::Empty;

        subtree['g'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 32);
            Some(ppt)
        };

        assert!(subtree['g'].is_some());
        assert!(subtree['c'].is_none());

        subtree['c'] = {
            let mut ppt = Box::new(PathPrefixTree::default());
            ppt.insert_tail("", 40);
            Some(ppt)
        };

        assert_eq!(subtree['g'].as_ref().unwrap().get(""), Some(&32));

        assert!(subtree['c'].is_some());
        assert_eq!(subtree['c'].as_ref().unwrap().get(""), Some(&40));

        subtree['c'] = None;
        assert!(subtree['c'].is_none());
        assert!(subtree['g'].is_some());
    }
}
