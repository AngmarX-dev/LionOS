//! Bounded in-memory filesystem core. No disk driver is implied by this module.
const NODE_COUNT:usize=64;
const NAME_LEN:usize=64;
const FILE_CAPACITY:usize=4096;
#[derive(Clone,Copy)]
struct Node{used:bool,name:[u8;NAME_LEN],len:usize,data:[u8;FILE_CAPACITY]}
impl Node{const EMPTY:Self=Self{used:false,name:[0;NAME_LEN],len:0,data:[0;FILE_CAPACITY]};}
static mut NODES:[Node;NODE_COUNT]=[Node::EMPTY;NODE_COUNT];
fn valid_name(name:&str)->bool{!name.is_empty()&&name.len()<NAME_LEN&&!name.contains("..")&&!name.contains('/')&&!name.contains('\\')}
fn matches(node:&Node,name:&str)->bool{node.used&&name.len()<NAME_LEN&&&node.name[..name.len()]==name.as_bytes()&&node.name[name.len()]==0}
pub fn create(name:&str)->bool{
    if !valid_name(name){return false;}
    unsafe{
        if NODES.iter().any(|n|matches(n,name)){return false;}
        let Some(slot)=NODES.iter_mut().find(|n|!n.used)else{return false;};
        slot.used=true;slot.name[..name.len()].copy_from_slice(name.as_bytes());slot.len=0;true
    }
}
pub fn write(name:&str,data:&[u8])->bool{
    if !valid_name(name)||data.len()>FILE_CAPACITY{return false;}
    unsafe{
        let Some(node)=NODES.iter_mut().find(|n|matches(n,name))else{return false;};
        node.data[..data.len()].copy_from_slice(data);node.len=data.len();true
    }
}
pub fn read(name:&str,output:&mut[u8])->Option<usize>{
    if !valid_name(name){return None;}
    unsafe{
        let node=NODES.iter().find(|n|matches(n,name))?;
        let count=output.len().min(node.len);output[..count].copy_from_slice(&node.data[..count]);Some(count)
    }
}
pub fn remove(name:&str)->bool{
    if !valid_name(name){return false;}
    unsafe{
        let Some(node)=NODES.iter_mut().find(|n|matches(n,name))else{return false;};
        *node=Node::EMPTY;true
    }
}
