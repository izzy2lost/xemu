use alloc::string::ToString;

use super::{PathCow, PathRef, PathVec};

impl<'a> serde::Serialize for PathRef<'a> {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        let string = self.to_string();
        serializer.serialize_str(&string)
    }
}

impl serde::Serialize for PathVec {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        let string = self.to_string();
        serializer.serialize_str(&string)
    }
}

impl<'a> serde::Serialize for PathCow<'a> {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        let string = self.path_ref().to_string();
        serializer.serialize_str(&string)
    }
}

struct PathVisitor;

impl<'de> serde::de::Visitor<'de> for PathVisitor {
    type Value = PathVec;

    fn expecting(&self, formatter: &mut std::fmt::Formatter) -> std::fmt::Result {
        formatter.write_str("a path with components separated by '/'")
    }

    fn visit_str<E>(self, v: &str) -> Result<Self::Value, E>
    where
        E: serde::de::Error,
    {
        Ok(PathVec::from(v))
    }
}

impl<'de> serde::Deserialize<'de> for PathVec {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        deserializer.deserialize_str(PathVisitor)
    }
}

impl<'a, 'de> serde::Deserialize<'de> for PathCow<'a> {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let path = deserializer.deserialize_str(PathVisitor)?;
        Ok(PathCow::Owned(path))
    }
}

#[cfg(test)]
mod test {
    use serde_test::{assert_ser_tokens, assert_tokens, Token};

    use crate::write::fs::{PathCow, PathRef, PathVec};

    #[test]
    fn test_pathvec_serde() {
        let path: PathRef = "/abc/def".into();
        let path: PathVec = path.into();

        assert_tokens(&path, &[Token::String("/abc/def")]);
    }

    #[test]
    fn test_pathcow_serde() {
        let path: PathRef = "/abc/def".into();
        let path: PathCow = path.into();

        assert_tokens(&path, &[Token::String("/abc/def")]);
    }

    #[test]
    fn test_pathref_serde() {
        let path: PathRef = "/abc/def".into();

        assert_ser_tokens(&path, &[Token::String("/abc/def")]);
    }
}
