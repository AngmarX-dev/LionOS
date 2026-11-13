//! Bounded hierarchical in-memory filesystem (RAMFS).
//!
//! The filesystem is deliberately allocation-free and has fixed limits. Paths
//! are UTF-8 byte strings, case-sensitive, and use '/' as the only separator.
//! It is a volatile filesystem: no block device or persistence is implied.

const NODE_COUNT: usize = 64;
const NAME_LEN: usize = 64;
const FILE_CAPACITY: usize = 4096;
const INVALID_NODE: usize = usize::MAX;

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum NodeKind { Free = 0, Directory = 1, File = 2 }

#[derive(Clone, Copy)]
struct Node {
    kind: NodeKind,
    parent: usize,
    name: [u8; NAME_LEN],
    len: usize,
    data: [u8; FILE_CAPACITY],
    generation: u32,
}

impl Node {
    const EMPTY: Self = Self {
        kind: NodeKind::Free, parent: INVALID_NODE, name: [0; NAME_LEN],
        len: 0, data: [0; FILE_CAPACITY], generation: 0,
    };
    const ROOT: Self = Self {
        kind: NodeKind::Directory, parent: 0, name: [0; NAME_LEN],
        len: 0, data: [0; FILE_CAPACITY], generation: 1,
    };
}

const fn initial_nodes() -> [Node; NODE_COUNT] {\n    let mut nodes = [Node::EMPTY; NODE_COUNT];\n    nodes[0] = Node::ROOT;\n    nodes\n}\n\nstatic mut NODES: [Node; NODE_COUNT] = initial_nodes();
static mut NEXT_GENERATION: u32 = 2;

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FsError {
    InvalidPath,
    NotFound,
    AlreadyExists,
    NotDirectory,
    IsDirectory,
    DirectoryNotEmpty,
    NoSpace,
    FileTooLarge,
    StaleHandle,
    BufferTooSmall,
}

#[derive(Clone, Copy)]
pub struct FileHandle { index: usize, generation: u32 }

#[derive(Clone, Copy)]
pub struct FileInfo {
    pub is_directory: bool,
    pub size: usize,
    pub generation: u32,
}

#[derive(Clone, Copy)]
pub struct DirEntry {
    pub name: [u8; NAME_LEN],
    pub name_len: usize,
    pub is_directory: bool,
    pub size: usize,
}

impl DirEntry {
    const EMPTY: Self = Self { name: [0; NAME_LEN], name_len: 0, is_directory: false, size: 0 };
}

fn valid_component(component: &str) -> bool {
    !component.is_empty() && component.len() < NAME_LEN
        && !component.contains('/') && !component.contains('\\')
        && component != "." && component != ".."
}

fn next_component(path: &str, mut offset: usize) -> Result<Option<(usize, usize)>, FsError> {
    let bytes = path.as_bytes();
    while offset < bytes.len() && bytes[offset] == b'/' { offset += 1; }
    if offset == bytes.len() { return Ok(None); }
    let start = offset;
    while offset < bytes.len() && bytes[offset] != b'/' { offset += 1; }
    let end = offset;
    let part = core::str::from_utf8(&bytes[start..end]).map_err(|_| FsError::InvalidPath)?;
    if part == ".." { return Err(FsError::InvalidPath); }
    if part != "." && !valid_component(part) { return Err(FsError::InvalidPath); }
    Ok(Some((start, end)))
}

fn find_child(parent: usize, name: &str) -> Option<usize> {
    unsafe {
        let mut i = 1;
        while i < NODE_COUNT {
            let node = &NODES[i];
            if node.kind != NodeKind::Free && node.parent == parent
                && &node.name[..name.len()] == name.as_bytes()
                && node.name[name.len()] == 0 { return Some(i); }
            i += 1;
        }
    }
    None
}

fn resolve(path: &str) -> Result<usize, FsError> {
    if path.is_empty() { return Err(FsError::InvalidPath); }
    let mut current = 0usize;
    let mut offset = 0usize;
    loop {
        match next_component(path, offset)? {
            None => return Ok(current),
            Some((start, end)) => {
                let part = core::str::from_utf8(&path.as_bytes()[start..end])
                    .map_err(|_| FsError::InvalidPath)?;
                if part != "." {
                    if unsafe { NODES[current].kind } != NodeKind::Directory {
                        return Err(FsError::NotDirectory);
                    }
                    current = find_child(current, part).ok_or(FsError::NotFound)?;
                }
                offset = end;
            }
        }
    }
}

fn resolve_parent(path: &str) -> Result<(usize, &str), FsError> {
    if path.is_empty() { return Err(FsError::InvalidPath); }
    let bytes = path.as_bytes();
    let mut end = bytes.len();
    while end > 0 && bytes[end - 1] == b'/' { end -= 1; }
    if end == 0 { return Err(FsError::InvalidPath); }
    let start = bytes[..end].iter().rposition(|b| *b == b'/').map_or(0, |p| p + 1);
    let name = core::str::from_utf8(&bytes[start..end]).map_err(|_| FsError::InvalidPath)?;
    if !valid_component(name) { return Err(FsError::InvalidPath); }
    let parent_path = if start == 0 { "/" } else { &path[..start] };
    let parent = resolve(parent_path)?;
    if unsafe { NODES[parent].kind } != NodeKind::Directory { return Err(FsError::NotDirectory); }
    Ok((parent, name))
}

fn allocate_node(parent: usize, name: &str, kind: NodeKind) -> Result<usize, FsError> {
    if find_child(parent, name).is_some() { return Err(FsError::AlreadyExists); }
    unsafe {
        let mut slot = 1usize;
        while slot < NODE_COUNT && NODES[slot].kind != NodeKind::Free { slot += 1; }
        if slot == NODE_COUNT { return Err(FsError::NoSpace); }
        let generation = NEXT_GENERATION;
        NEXT_GENERATION = NEXT_GENERATION.wrapping_add(1).max(2);
        let mut node = Node::EMPTY;
        node.kind = kind;
        node.parent = parent;
        node.name[..name.len()].copy_from_slice(name.as_bytes());
        node.generation = generation;
        NODES[slot] = node;
        Ok(slot)
    }
}

pub fn create_dir(path: &str) -> Result<(), FsError> {
    let (parent, name) = resolve_parent(path)?;
    allocate_node(parent, name, NodeKind::Directory).map(|_| ())
}

pub fn create_file(path: &str) -> Result<FileHandle, FsError> {
    let (parent, name) = resolve_parent(path)?;
    let index = allocate_node(parent, name, NodeKind::File)?;
    unsafe { Ok(FileHandle { index, generation: NODES[index].generation }) }
}

pub fn open_file(path: &str) -> Result<FileHandle, FsError> {
    let index = resolve(path)?;
    unsafe {
        if NODES[index].kind == NodeKind::Directory { return Err(FsError::IsDirectory); }
        Ok(FileHandle { index, generation: NODES[index].generation })
    }
}

fn checked_file(handle: FileHandle) -> Result<usize, FsError> {
    if handle.index == 0 || handle.index >= NODE_COUNT { return Err(FsError::StaleHandle); }
    unsafe {
        let node = &NODES[handle.index];
        if node.kind != NodeKind::File || node.generation != handle.generation {
            return Err(FsError::StaleHandle);
        }
    }
    Ok(handle.index)
}

pub fn write_handle(handle: FileHandle, data: &[u8]) -> Result<(), FsError> {
    if data.len() > FILE_CAPACITY { return Err(FsError::FileTooLarge); }
    let index = checked_file(handle)?;
    unsafe {
        NODES[index].data[..data.len()].copy_from_slice(data);
        NODES[index].len = data.len();
    }
    Ok(())
}

pub fn append_handle(handle: FileHandle, data: &[u8]) -> Result<(), FsError> {
    let index = checked_file(handle)?;
    unsafe {
        let old_len = NODES[index].len;
        let new_len = old_len.checked_add(data.len()).ok_or(FsError::FileTooLarge)?;
        if new_len > FILE_CAPACITY { return Err(FsError::FileTooLarge); }
        NODES[index].data[old_len..new_len].copy_from_slice(data);
        NODES[index].len = new_len;
    }
    Ok(())
}

pub fn read_handle(handle: FileHandle, output: &mut [u8]) -> Result<usize, FsError> {
    let index = checked_file(handle)?;
    unsafe {
        let count = output.len().min(NODES[index].len);
        output[..count].copy_from_slice(&NODES[index].data[..count]);
        Ok(count)
    }
}

pub fn write_file(path: &str, data: &[u8]) -> Result<(), FsError> {
    let handle = match open_file(path) {
        Ok(handle) => handle,
        Err(FsError::NotFound) => create_file(path)?,
        Err(error) => return Err(error),
    };
    write_handle(handle, data)
}

pub fn append_file(path: &str, data: &[u8]) -> Result<(), FsError> {
    let handle = open_file(path)?;
    append_handle(handle, data)
}

pub fn read_file(path: &str, output: &mut [u8]) -> Result<usize, FsError> {
    read_handle(open_file(path)?, output)
}

pub fn stat(path: &str) -> Result<FileInfo, FsError> {
    let index = resolve(path)?;
    unsafe {
        let node = &NODES[index];
        Ok(FileInfo {
            is_directory: node.kind == NodeKind::Directory,
            size: node.len,
            generation: node.generation,
        })
    }
}

/// Return the Nth live child of a directory. Repeated calls with increasing
/// indices provide a bounded directory iterator without allocating memory.
pub fn read_dir(path: &str, ordinal: usize, output: &mut DirEntry) -> Result<bool, FsError> {
    let parent = resolve(path)?;
    if unsafe { NODES[parent].kind } != NodeKind::Directory { return Err(FsError::NotDirectory); }
    let mut seen = 0usize;
    let mut i = 1usize;
    while i < NODE_COUNT {
        unsafe {
            let node = &NODES[i];
            if node.kind != NodeKind::Free && node.parent == parent {
                if seen == ordinal {
                    *output = DirEntry::EMPTY;
                    output.name = node.name;
                    output.name_len = node.name.iter().position(|b| *b == 0).unwrap_or(NAME_LEN);
                    output.is_directory = node.kind == NodeKind::Directory;
                    output.size = node.len;
                    return Ok(true);
                }
                seen += 1;
            }
        }
        i += 1;
    }
    *output = DirEntry::EMPTY;
    Ok(false)
}

pub fn remove_path(path: &str) -> Result<(), FsError> {
    let index = resolve(path)?;
    if index == 0 { return Err(FsError::InvalidPath); }
    unsafe {
        if NODES[index].kind == NodeKind::Directory {
            let mut i = 1usize;
            while i < NODE_COUNT {
                if NODES[i].kind != NodeKind::Free && NODES[i].parent == index {
                    return Err(FsError::DirectoryNotEmpty);
                }
                i += 1;
            }
        }
        NODES[index] = Node::EMPTY;
    }
    Ok(())
}

pub fn capacity() -> (usize, usize, usize) {
    let mut used = 0usize;
    let mut files = 0usize;
    let mut directories = 0usize;
    unsafe {
        let mut i = 1usize;
        while i < NODE_COUNT {
            match NODES[i].kind {
                NodeKind::File => { used += 1; files += 1; }
                NodeKind::Directory => { used += 1; directories += 1; }
                NodeKind::Free => {}
            }
            i += 1;
        }
    }
    (used, files, directories)
}

// Compatibility wrappers used by the original boot path. These retain the
// old flat-name API while routing storage through the hierarchical filesystem.
pub fn create(name: &str) -> bool { create_file(name).is_ok() }
pub fn write(name: &str, data: &[u8]) -> bool { write_file(name, data).is_ok() }
pub fn read(name: &str, output: &mut [u8]) -> Option<usize> { read_file(name, output).ok() }
pub fn remove(name: &str) -> bool { remove_path(name).is_ok() }
