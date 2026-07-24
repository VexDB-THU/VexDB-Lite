use async_trait::async_trait;
use nfsserve::nfs::{
    fattr3, fileid3, filename3, ftype3, nfspath3, nfsstat3, nfstime3, sattr3, set_atime, set_gid3,
    set_mode3, set_mtime, set_size3, set_uid3, specdata3,
};
use nfsserve::tcp::{NFSTcp, NFSTcpListener};
use nfsserve::vfs::{DirEntry, NFSFileSystem, ReadDirResult, VFSCapabilities};
use serde::Deserialize;
use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

const RUNTIME_ABI_VERSION: u32 = 1;
const RUNTIME_EXCLUSIVE_GATEWAY: u32 = 2;
const XATTR_ALWAYS_SET: c_int = 0;
const XATTR_DELETE: c_int = 3;
const TIME_ACCESS: u32 = 1;
const TIME_MODIFY: u32 = 2;
const APPLEDOUBLE_XATTR: &str = "io.vexdb.macos.appledouble";
const SIDECAR_INODE_START: u64 = u64::MAX - 1024;

#[repr(C)]
pub struct VexfsNfsGatewayConfig {
    abi_version: u32,
    backend: *const c_char,
    connection: *const c_char,
    workspace: *const c_char,
    principal: *const c_char,
    listen_address: *const c_char,
    port: u16,
    operation_timeout_ms: u32,
}

#[repr(C)]
struct RuntimeConfig {
    abi_version: u32,
    backend: *const c_char,
    connection: *const c_char,
    workspace: *const c_char,
    principal: *const c_char,
    operation_timeout_ms: u32,
    flags: u32,
}

#[repr(C)]
struct RuntimeBytes {
    data: *mut c_void,
    size: u64,
}

#[repr(C)]
struct RuntimeError {
    status: c_int,
    native_code: c_int,
    backend: [c_char; 16],
    message: [c_char; 512],
}

enum RuntimeSessionOpaque {}
type RuntimeStatus = c_int;

extern "C" {
    fn vexfs_mount_session_open(
        config: *const RuntimeConfig,
        session: *mut *mut RuntimeSessionOpaque,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_session_close(session: *mut RuntimeSessionOpaque);
    fn vexfs_mount_stat(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        json: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_path_for_inode(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        path: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_list(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        json: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_read_file(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        content: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_mkdir(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_set_mode(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        mode: u32,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_set_times(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        accessed_at_ms: i64,
        modified_at_ms: i64,
        mask: u32,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_chown(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        uid: i64,
        gid: i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_symlink(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        target: *const c_void,
        target_size: u64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_readlink(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        target: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_rename(
        session: *mut RuntimeSessionOpaque,
        source: *const c_char,
        destination: *const c_char,
        replace: c_int,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_link(
        session: *mut RuntimeSessionOpaque,
        source: *const c_char,
        destination: *const c_char,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_remove(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        recursive: c_int,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_xattr_get(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        name: *const c_char,
        value: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_xattr_set(
        session: *mut RuntimeSessionOpaque,
        inode: i64,
        name: *const c_char,
        value: *const c_void,
        size: u64,
        policy: c_int,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_open(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        flags: *const c_char,
        request_id: *const c_char,
        handle: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_create(
        session: *mut RuntimeSessionOpaque,
        path: *const c_char,
        mode: u32,
        request_id: *const c_char,
        handle: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_stage_write(
        session: *mut RuntimeSessionOpaque,
        handle: *const c_char,
        offset: u64,
        data: *const c_void,
        size: u64,
        request_id: *const c_char,
        generation: *mut i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_truncate(
        session: *mut RuntimeSessionOpaque,
        handle: *const c_char,
        size: u64,
        request_id: *const c_char,
        generation: *mut i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_publish_close(
        session: *mut RuntimeSessionOpaque,
        handle: *const c_char,
        generation: i64,
        durability: *const c_char,
        version: *mut i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_handle_close(
        session: *mut RuntimeSessionOpaque,
        handle: *const c_char,
        retain_unpublished: c_int,
        request_id: *const c_char,
        state: *mut RuntimeBytes,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_synchronize(
        session: *mut RuntimeSessionOpaque,
        request_id: *const c_char,
        published: *mut i64,
        error: *mut RuntimeError,
    ) -> RuntimeStatus;
    fn vexfs_mount_free(memory: *mut c_void);
}

#[derive(Clone, Deserialize)]
struct FileStat {
    inode: i64,
    kind: String,
    mode: u32,
    uid: i64,
    gid: i64,
    size: i64,
    link_count: i64,
    accessed_at: i64,
    updated_at: i64,
    changed_at: i64,
}

#[derive(Clone, Deserialize)]
struct ListEntry {
    name: String,
    inode: i64,
    kind: String,
    mode: u32,
    uid: i64,
    gid: i64,
    size: i64,
    link_count: i64,
    accessed_at: i64,
    updated_at: i64,
    changed_at: i64,
}

impl From<ListEntry> for FileStat {
    fn from(entry: ListEntry) -> Self {
        Self {
            inode: entry.inode,
            kind: entry.kind,
            mode: entry.mode,
            uid: entry.uid,
            gid: entry.gid,
            size: entry.size,
            link_count: entry.link_count,
            accessed_at: entry.accessed_at,
            updated_at: entry.updated_at,
            changed_at: entry.changed_at,
        }
    }
}

#[derive(Clone, Hash, PartialEq, Eq)]
struct SidecarKey {
    parent: fileid3,
    name: Vec<u8>,
}

struct Sidecar {
    key: SidecarKey,
    target_inode: Option<fileid3>,
    data: Vec<u8>,
    mode: u32,
    updated_at_ms: i64,
}

struct RuntimeState {
    session: *mut RuntimeSessionOpaque,
    root_inode: fileid3,
    uid: u32,
    gid: u32,
    next_sidecar_inode: fileid3,
    sidecars_by_key: HashMap<SidecarKey, fileid3>,
    sidecars: HashMap<fileid3, Sidecar>,
}

unsafe impl Send for RuntimeState {}

impl Drop for RuntimeState {
    fn drop(&mut self) {
        if !self.session.is_null() {
            unsafe { vexfs_mount_session_close(self.session) };
            self.session = ptr::null_mut();
        }
    }
}

pub struct VexfsNfs {
    state: Mutex<RuntimeState>,
    request_prefix: String,
    request_sequence: AtomicU64,
}

fn empty_error() -> RuntimeError {
    RuntimeError {
        status: 0,
        native_code: 0,
        backend: [0; 16],
        message: [0; 512],
    }
}

fn runtime_to_nfs(status: RuntimeStatus) -> nfsstat3 {
    match status {
        1 => nfsstat3::NFS3ERR_INVAL,
        2 => nfsstat3::NFS3ERR_NOENT,
        3 => nfsstat3::NFS3ERR_EXIST,
        4 => nfsstat3::NFS3ERR_ROFS,
        5 => nfsstat3::NFS3ERR_JUKEBOX,
        8 => nfsstat3::NFS3ERR_ACCES,
        9 => nfsstat3::NFS3ERR_NOSPC,
        11 => nfsstat3::NFS3ERR_NOTSUPP,
        12 => nfsstat3::NFS3ERR_NOTEMPTY,
        _ => nfsstat3::NFS3ERR_IO,
    }
}

fn checked_cstring(value: &str) -> Result<CString, nfsstat3> {
    CString::new(value).map_err(|_| nfsstat3::NFS3ERR_INVAL)
}

fn bytes_to_string(bytes: RuntimeBytes) -> Result<String, nfsstat3> {
    let value = unsafe {
        if bytes.data.is_null() {
            Vec::new()
        } else {
            let value =
                std::slice::from_raw_parts(bytes.data.cast::<u8>(), bytes.size as usize).to_vec();
            vexfs_mount_free(bytes.data);
            value
        }
    };
    String::from_utf8(value).map_err(|_| nfsstat3::NFS3ERR_IO)
}

fn take_bytes(bytes: RuntimeBytes) -> Vec<u8> {
    unsafe {
        if bytes.data.is_null() {
            Vec::new()
        } else {
            let value =
                std::slice::from_raw_parts(bytes.data.cast::<u8>(), bytes.size as usize).to_vec();
            vexfs_mount_free(bytes.data);
            value
        }
    }
}

fn runtime_result(status: RuntimeStatus) -> Result<(), nfsstat3> {
    if status == 0 {
        Ok(())
    } else {
        Err(runtime_to_nfs(status))
    }
}

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
        .min(i64::MAX as u128) as i64
}

fn nfs_time(milliseconds: i64) -> nfstime3 {
    let value = milliseconds.max(0) as u64;
    nfstime3 {
        seconds: (value / 1000).min(u32::MAX as u64) as u32,
        nseconds: ((value % 1000) * 1_000_000) as u32,
    }
}

fn stat_attr(stat: &FileStat) -> fattr3 {
    let ftype = match stat.kind.as_str() {
        "directory" => ftype3::NF3DIR,
        "symlink" => ftype3::NF3LNK,
        _ => ftype3::NF3REG,
    };
    fattr3 {
        ftype,
        mode: stat.mode & 0o7777,
        nlink: stat.link_count.max(1).min(u32::MAX as i64) as u32,
        uid: stat.uid.max(0).min(u32::MAX as i64) as u32,
        gid: stat.gid.max(0).min(u32::MAX as i64) as u32,
        size: stat.size.max(0) as u64,
        used: stat.size.max(0) as u64,
        rdev: specdata3::default(),
        fsid: 1,
        fileid: stat.inode as u64,
        atime: nfs_time(stat.accessed_at),
        mtime: nfs_time(stat.updated_at),
        ctime: nfs_time(stat.changed_at),
    }
}

impl RuntimeState {
    fn stat_path(&mut self, path: &str) -> Result<FileStat, nfsstat3> {
        let path = checked_cstring(path)?;
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status =
            unsafe { vexfs_mount_stat(self.session, path.as_ptr(), &mut bytes, &mut error) };
        runtime_result(status)?;
        serde_json::from_str(&bytes_to_string(bytes)?).map_err(|_| nfsstat3::NFS3ERR_IO)
    }

    fn path_for_inode(&mut self, inode: fileid3) -> Result<String, nfsstat3> {
        if self.sidecars.contains_key(&inode) {
            return Err(nfsstat3::NFS3ERR_INVAL);
        }
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_path_for_inode(self.session, inode as i64, &mut bytes, &mut error)
        };
        runtime_result(status)?;
        bytes_to_string(bytes)
    }

    fn child_path(&mut self, parent: fileid3, name: &[u8]) -> Result<String, nfsstat3> {
        if name.is_empty() || name.contains(&b'/') || name.contains(&0) {
            return Err(nfsstat3::NFS3ERR_INVAL);
        }
        let name = std::str::from_utf8(name).map_err(|_| nfsstat3::NFS3ERR_INVAL)?;
        let parent = self.path_for_inode(parent)?;
        Ok(if parent == "/" {
            format!("/{name}")
        } else {
            format!("{parent}/{name}")
        })
    }

    fn list(&mut self, path: &str) -> Result<Vec<ListEntry>, nfsstat3> {
        let path = checked_cstring(path)?;
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status =
            unsafe { vexfs_mount_list(self.session, path.as_ptr(), &mut bytes, &mut error) };
        runtime_result(status)?;
        serde_json::from_str(&bytes_to_string(bytes)?).map_err(|_| nfsstat3::NFS3ERR_IO)
    }

    fn read_file(&mut self, path: &str) -> Result<Vec<u8>, nfsstat3> {
        let path = checked_cstring(path)?;
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status =
            unsafe { vexfs_mount_read_file(self.session, path.as_ptr(), &mut bytes, &mut error) };
        runtime_result(status)?;
        Ok(take_bytes(bytes))
    }

    fn request_id(prefix: &str, sequence: &AtomicU64, operation: &str) -> CString {
        CString::new(format!(
            "{prefix}-{operation}-{}",
            sequence.fetch_add(1, Ordering::Relaxed)
        ))
        .expect("request id has no NUL")
    }

    fn create_file(
        &mut self,
        path: &str,
        mode: u32,
        prefix: &str,
        sequence: &AtomicU64,
    ) -> Result<(), nfsstat3> {
        let path = checked_cstring(path)?;
        let request = Self::request_id(prefix, sequence, "create");
        let mut handle = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_handle_create(
                self.session,
                path.as_ptr(),
                mode & 0o7777,
                request.as_ptr(),
                &mut handle,
                &mut error,
            )
        };
        runtime_result(status)?;
        let handle = checked_cstring(&bytes_to_string(handle)?)?;
        let durability = CString::new("full").unwrap();
        let mut version = 0;
        let status = unsafe {
            vexfs_mount_handle_publish_close(
                self.session,
                handle.as_ptr(),
                1,
                durability.as_ptr(),
                &mut version,
                &mut error,
            )
        };
        if status != 0 {
            self.close_failed_handle(&handle, prefix, sequence);
        }
        runtime_result(status)
    }

    fn write_file(
        &mut self,
        path: &str,
        offset: u64,
        data: &[u8],
        prefix: &str,
        sequence: &AtomicU64,
    ) -> Result<(), nfsstat3> {
        let path = checked_cstring(path)?;
        let mode = CString::new("rw").unwrap();
        let open_request = Self::request_id(prefix, sequence, "open-write");
        let mut handle = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_handle_open(
                self.session,
                path.as_ptr(),
                mode.as_ptr(),
                open_request.as_ptr(),
                &mut handle,
                &mut error,
            )
        };
        runtime_result(status)?;
        let handle = checked_cstring(&bytes_to_string(handle)?)?;
        let write_request = Self::request_id(prefix, sequence, "write");
        let mut generation = 0;
        let status = unsafe {
            vexfs_mount_handle_stage_write(
                self.session,
                handle.as_ptr(),
                offset,
                data.as_ptr().cast(),
                data.len() as u64,
                write_request.as_ptr(),
                &mut generation,
                &mut error,
            )
        };
        if status != 0 {
            self.close_failed_handle(&handle, prefix, sequence);
            return Err(runtime_to_nfs(status));
        }
        let durability = CString::new("full").unwrap();
        let mut version = 0;
        let status = unsafe {
            vexfs_mount_handle_publish_close(
                self.session,
                handle.as_ptr(),
                generation,
                durability.as_ptr(),
                &mut version,
                &mut error,
            )
        };
        if status != 0 {
            self.close_failed_handle(&handle, prefix, sequence);
        }
        runtime_result(status)
    }

    fn truncate_file(
        &mut self,
        path: &str,
        size: u64,
        prefix: &str,
        sequence: &AtomicU64,
    ) -> Result<(), nfsstat3> {
        let path = checked_cstring(path)?;
        let mode = CString::new("rw").unwrap();
        let open_request = Self::request_id(prefix, sequence, "open-truncate");
        let mut handle = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_handle_open(
                self.session,
                path.as_ptr(),
                mode.as_ptr(),
                open_request.as_ptr(),
                &mut handle,
                &mut error,
            )
        };
        runtime_result(status)?;
        let handle = checked_cstring(&bytes_to_string(handle)?)?;
        let request = Self::request_id(prefix, sequence, "truncate");
        let mut generation = 0;
        let status = unsafe {
            vexfs_mount_handle_truncate(
                self.session,
                handle.as_ptr(),
                size,
                request.as_ptr(),
                &mut generation,
                &mut error,
            )
        };
        if status != 0 {
            self.close_failed_handle(&handle, prefix, sequence);
            return Err(runtime_to_nfs(status));
        }
        let durability = CString::new("full").unwrap();
        let mut version = 0;
        let status = unsafe {
            vexfs_mount_handle_publish_close(
                self.session,
                handle.as_ptr(),
                generation,
                durability.as_ptr(),
                &mut version,
                &mut error,
            )
        };
        if status != 0 {
            self.close_failed_handle(&handle, prefix, sequence);
        }
        runtime_result(status)
    }

    fn close_failed_handle(&mut self, handle: &CString, prefix: &str, sequence: &AtomicU64) {
        let request = Self::request_id(prefix, sequence, "close-failed");
        let mut state = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        unsafe {
            vexfs_mount_handle_close(
                self.session,
                handle.as_ptr(),
                1,
                request.as_ptr(),
                &mut state,
                &mut error,
            );
        }
        let _ = take_bytes(state);
    }

    fn target_name(sidecar_name: &[u8]) -> Option<&[u8]> {
        sidecar_name
            .strip_prefix(b"._")
            .filter(|name| !name.is_empty() && *name != b"." && *name != b"..")
    }

    fn sidecar_target(
        &mut self,
        parent: fileid3,
        sidecar_name: &[u8],
    ) -> Result<(String, fileid3), nfsstat3> {
        let target_name = Self::target_name(sidecar_name).ok_or(nfsstat3::NFS3ERR_NOENT)?;
        let path = self.child_path(parent, target_name)?;
        let stat = self.stat_path(&path)?;
        Ok((path, stat.inode as u64))
    }

    fn get_xattr(&mut self, inode: fileid3, name: &str) -> Result<Vec<u8>, nfsstat3> {
        let name = checked_cstring(name)?;
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_xattr_get(
                self.session,
                inode as i64,
                name.as_ptr(),
                &mut bytes,
                &mut error,
            )
        };
        runtime_result(status)?;
        Ok(take_bytes(bytes))
    }

    fn set_xattr(
        &mut self,
        inode: fileid3,
        name: &str,
        data: &[u8],
        policy: c_int,
    ) -> Result<(), nfsstat3> {
        let name = checked_cstring(name)?;
        let mut error = empty_error();
        let status = unsafe {
            vexfs_mount_xattr_set(
                self.session,
                inode as i64,
                name.as_ptr(),
                data.as_ptr().cast(),
                data.len() as u64,
                policy,
                &mut error,
            )
        };
        runtime_result(status)
    }

    fn allocate_sidecar(
        &mut self,
        key: SidecarKey,
        target_inode: Option<fileid3>,
        data: Vec<u8>,
        mode: u32,
    ) -> fileid3 {
        if let Some(inode) = self.sidecars_by_key.get(&key) {
            return *inode;
        }
        let inode = self.next_sidecar_inode;
        self.next_sidecar_inode = self.next_sidecar_inode.saturating_sub(1);
        self.sidecars_by_key.insert(key.clone(), inode);
        self.sidecars.insert(
            inode,
            Sidecar {
                key,
                target_inode,
                data,
                mode,
                updated_at_ms: now_ms(),
            },
        );
        inode
    }

    fn lookup_sidecar(&mut self, parent: fileid3, name: &[u8]) -> Result<fileid3, nfsstat3> {
        let key = SidecarKey {
            parent,
            name: name.to_vec(),
        };
        if let Some(inode) = self.sidecars_by_key.get(&key) {
            return Ok(*inode);
        }
        let (_, target_inode) = self.sidecar_target(parent, name)?;
        let data = self.get_xattr(target_inode, APPLEDOUBLE_XATTR)?;
        Ok(self.allocate_sidecar(key, Some(target_inode), data, 0o600))
    }

    fn sidecar_attr(&self, inode: fileid3) -> Result<fattr3, nfsstat3> {
        let sidecar = self.sidecars.get(&inode).ok_or(nfsstat3::NFS3ERR_STALE)?;
        Ok(fattr3 {
            ftype: ftype3::NF3REG,
            mode: sidecar.mode,
            nlink: 1,
            uid: self.uid,
            gid: self.gid,
            size: sidecar.data.len() as u64,
            used: sidecar.data.len() as u64,
            rdev: specdata3::default(),
            fsid: 1,
            fileid: inode,
            atime: nfs_time(sidecar.updated_at_ms),
            mtime: nfs_time(sidecar.updated_at_ms),
            ctime: nfs_time(sidecar.updated_at_ms),
        })
    }

    fn persist_sidecar(&mut self, inode: fileid3) -> Result<(), nfsstat3> {
        let (key, target_inode, data) = {
            let sidecar = self.sidecars.get(&inode).ok_or(nfsstat3::NFS3ERR_STALE)?;
            (
                sidecar.key.clone(),
                sidecar.target_inode,
                sidecar.data.clone(),
            )
        };
        let target_inode = match target_inode {
            Some(value) => value,
            None => self.sidecar_target(key.parent, &key.name)?.1,
        };
        self.set_xattr(target_inode, APPLEDOUBLE_XATTR, &data, XATTR_ALWAYS_SET)?;
        if let Some(sidecar) = self.sidecars.get_mut(&inode) {
            sidecar.target_inode = Some(target_inode);
        }
        Ok(())
    }

    fn remove_sidecar(&mut self, parent: fileid3, name: &[u8]) -> Result<(), nfsstat3> {
        let key = SidecarKey {
            parent,
            name: name.to_vec(),
        };
        let inode = self.sidecars_by_key.remove(&key);
        let target_inode = inode
            .and_then(|value| self.sidecars.remove(&value))
            .and_then(|value| value.target_inode)
            .or_else(|| self.sidecar_target(parent, name).ok().map(|value| value.1));
        let Some(target_inode) = target_inode else {
            return Err(nfsstat3::NFS3ERR_NOENT);
        };
        match self.set_xattr(target_inode, APPLEDOUBLE_XATTR, &[], XATTR_DELETE) {
            Ok(()) | Err(nfsstat3::NFS3ERR_NOENT) => Ok(()),
            Err(status) => Err(status),
        }
    }
}

impl VexfsNfs {
    fn request_id(&self, operation: &str) -> CString {
        RuntimeState::request_id(&self.request_prefix, &self.request_sequence, operation)
    }
}

impl Drop for VexfsNfs {
    fn drop(&mut self) {
        let request = self.request_id("shutdown-sync");
        let Ok(state) = self.state.lock() else { return };
        let mut published = 0;
        let mut error = empty_error();
        unsafe {
            vexfs_mount_synchronize(state.session, request.as_ptr(), &mut published, &mut error);
        }
    }
}

#[async_trait]
impl NFSFileSystem for VexfsNfs {
    fn capabilities(&self) -> VFSCapabilities {
        VFSCapabilities::ReadWrite
    }

    fn root_dir(&self) -> fileid3 {
        self.state.lock().expect("runtime mutex").root_inode
    }

    async fn lookup(&self, dirid: fileid3, filename: &filename3) -> Result<fileid3, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if filename.as_ref() == b"." {
            return Ok(dirid);
        }
        if filename.as_ref() == b".." {
            let path = state.path_for_inode(dirid)?;
            if path == "/" {
                return Ok(dirid);
            }
            let parent = path.rsplit_once('/').map(|value| value.0).unwrap_or("");
            let parent = if parent.is_empty() { "/" } else { parent };
            return Ok(state.stat_path(parent)?.inode as u64);
        }
        if RuntimeState::target_name(filename.as_ref()).is_some() {
            return state.lookup_sidecar(dirid, filename.as_ref());
        }
        let path = state.child_path(dirid, filename.as_ref())?;
        Ok(state.stat_path(&path)?.inode as u64)
    }

    async fn getattr(&self, id: fileid3) -> Result<fattr3, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if state.sidecars.contains_key(&id) {
            return state.sidecar_attr(id);
        }
        let path = state.path_for_inode(id)?;
        Ok(stat_attr(&state.stat_path(&path)?))
    }

    async fn setattr(&self, id: fileid3, attrs: sattr3) -> Result<fattr3, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if state.sidecars.contains_key(&id) {
            if let set_mode3::mode(mode) = attrs.mode {
                state.sidecars.get_mut(&id).unwrap().mode = mode & 0o7777;
            }
            if let set_size3::size(size) = attrs.size {
                state
                    .sidecars
                    .get_mut(&id)
                    .unwrap()
                    .data
                    .resize(size as usize, 0);
                state.persist_sidecar(id)?;
            }
            return state.sidecar_attr(id);
        }
        let path = state.path_for_inode(id)?;
        let mut error = empty_error();
        if let set_mode3::mode(mode) = attrs.mode {
            runtime_result(unsafe {
                vexfs_mount_set_mode(state.session, id as i64, mode & 0o7777, &mut error)
            })?;
        }
        let uid = match attrs.uid {
            set_uid3::uid(value) => value as i64,
            set_uid3::Void => -1,
        };
        let gid = match attrs.gid {
            set_gid3::gid(value) => value as i64,
            set_gid3::Void => -1,
        };
        if uid >= 0 || gid >= 0 {
            runtime_result(unsafe {
                vexfs_mount_chown(state.session, id as i64, uid, gid, &mut error)
            })?;
        }
        if let set_size3::size(size) = attrs.size {
            state.truncate_file(&path, size, &self.request_prefix, &self.request_sequence)?;
        }
        let now = now_ms();
        let (accessed, access_mask) = match attrs.atime {
            set_atime::DONT_CHANGE => (0, 0),
            set_atime::SET_TO_SERVER_TIME => (now, TIME_ACCESS),
            set_atime::SET_TO_CLIENT_TIME(value) => (
                (value.seconds as i64) * 1000 + (value.nseconds as i64) / 1_000_000,
                TIME_ACCESS,
            ),
        };
        let (modified, modify_mask) = match attrs.mtime {
            set_mtime::DONT_CHANGE => (0, 0),
            set_mtime::SET_TO_SERVER_TIME => (now, TIME_MODIFY),
            set_mtime::SET_TO_CLIENT_TIME(value) => (
                (value.seconds as i64) * 1000 + (value.nseconds as i64) / 1_000_000,
                TIME_MODIFY,
            ),
        };
        if access_mask | modify_mask != 0 {
            runtime_result(unsafe {
                vexfs_mount_set_times(
                    state.session,
                    id as i64,
                    accessed,
                    modified,
                    access_mask | modify_mask,
                    &mut error,
                )
            })?;
        }
        Ok(stat_attr(&state.stat_path(&path)?))
    }

    async fn read(
        &self,
        id: fileid3,
        offset: u64,
        count: u32,
    ) -> Result<(Vec<u8>, bool), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let data = if let Some(sidecar) = state.sidecars.get(&id) {
            sidecar.data.clone()
        } else {
            let path = state.path_for_inode(id)?;
            state.read_file(&path)?
        };
        let start = (offset as usize).min(data.len());
        let end = start.saturating_add(count as usize).min(data.len());
        Ok((data[start..end].to_vec(), end == data.len()))
    }

    async fn write(&self, id: fileid3, offset: u64, data: &[u8]) -> Result<fattr3, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if state.sidecars.contains_key(&id) {
            let sidecar = state.sidecars.get_mut(&id).unwrap();
            let offset = offset as usize;
            if offset.saturating_add(data.len()) > sidecar.data.len() {
                sidecar.data.resize(offset.saturating_add(data.len()), 0);
            }
            sidecar.data[offset..offset + data.len()].copy_from_slice(data);
            sidecar.updated_at_ms = now_ms();
            state.persist_sidecar(id)?;
            return state.sidecar_attr(id);
        }
        let path = state.path_for_inode(id)?;
        state.write_file(
            &path,
            offset,
            data,
            &self.request_prefix,
            &self.request_sequence,
        )?;
        Ok(stat_attr(&state.stat_path(&path)?))
    }

    async fn create(
        &self,
        dirid: fileid3,
        filename: &filename3,
        attrs: sattr3,
    ) -> Result<(fileid3, fattr3), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let mode = match attrs.mode {
            set_mode3::mode(value) => value,
            set_mode3::Void => 0o644,
        };
        if RuntimeState::target_name(filename.as_ref()).is_some() {
            let key = SidecarKey {
                parent: dirid,
                name: filename.as_ref().to_vec(),
            };
            let target_inode = state
                .sidecar_target(dirid, filename.as_ref())
                .ok()
                .map(|value| value.1);
            let inode = state.allocate_sidecar(key, target_inode, Vec::new(), mode);
            return Ok((inode, state.sidecar_attr(inode)?));
        }
        let path = state.child_path(dirid, filename.as_ref())?;
        state.create_file(&path, mode, &self.request_prefix, &self.request_sequence)?;
        let stat = state.stat_path(&path)?;
        let mut error = empty_error();
        runtime_result(unsafe {
            vexfs_mount_chown(
                state.session,
                stat.inode,
                state.uid as i64,
                state.gid as i64,
                &mut error,
            )
        })?;
        let stat = state.stat_path(&path)?;
        Ok((stat.inode as u64, stat_attr(&stat)))
    }

    async fn create_exclusive(
        &self,
        dirid: fileid3,
        filename: &filename3,
    ) -> Result<fileid3, nfsstat3> {
        Ok(self.create(dirid, filename, sattr3::default()).await?.0)
    }

    async fn mkdir(
        &self,
        dirid: fileid3,
        dirname: &filename3,
    ) -> Result<(fileid3, fattr3), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let path = state.child_path(dirid, dirname.as_ref())?;
        let path_c = checked_cstring(&path)?;
        let mut error = empty_error();
        runtime_result(unsafe { vexfs_mount_mkdir(state.session, path_c.as_ptr(), &mut error) })?;
        let stat = state.stat_path(&path)?;
        let mode = 0o755;
        runtime_result(unsafe {
            vexfs_mount_set_mode(state.session, stat.inode, mode & 0o7777, &mut error)
        })?;
        runtime_result(unsafe {
            vexfs_mount_chown(
                state.session,
                stat.inode,
                state.uid as i64,
                state.gid as i64,
                &mut error,
            )
        })?;
        let stat = state.stat_path(&path)?;
        Ok((stat.inode as u64, stat_attr(&stat)))
    }

    async fn remove(&self, dirid: fileid3, filename: &filename3) -> Result<(), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if RuntimeState::target_name(filename.as_ref()).is_some() {
            return state.remove_sidecar(dirid, filename.as_ref());
        }
        let path = state.child_path(dirid, filename.as_ref())?;
        let path = checked_cstring(&path)?;
        let mut error = empty_error();
        runtime_result(unsafe { vexfs_mount_remove(state.session, path.as_ptr(), 0, &mut error) })
    }

    async fn rename(
        &self,
        from_dirid: fileid3,
        from_filename: &filename3,
        to_dirid: fileid3,
        to_filename: &filename3,
    ) -> Result<(), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let from_sidecar = RuntimeState::target_name(from_filename.as_ref()).is_some();
        let to_sidecar = RuntimeState::target_name(to_filename.as_ref()).is_some();
        if from_sidecar || to_sidecar {
            if !from_sidecar || !to_sidecar {
                return Err(nfsstat3::NFS3ERR_INVAL);
            }
            let inode = state.lookup_sidecar(from_dirid, from_filename.as_ref())?;
            let data = state.sidecars.get(&inode).unwrap().data.clone();
            state.remove_sidecar(from_dirid, from_filename.as_ref())?;
            let key = SidecarKey {
                parent: to_dirid,
                name: to_filename.as_ref().to_vec(),
            };
            let target = state
                .sidecar_target(to_dirid, to_filename.as_ref())
                .ok()
                .map(|value| value.1);
            let new_inode = state.allocate_sidecar(key, target, data, 0o600);
            return state.persist_sidecar(new_inode);
        }
        let source = checked_cstring(&state.child_path(from_dirid, from_filename.as_ref())?)?;
        let destination = checked_cstring(&state.child_path(to_dirid, to_filename.as_ref())?)?;
        let mut error = empty_error();
        runtime_result(unsafe {
            vexfs_mount_rename(
                state.session,
                source.as_ptr(),
                destination.as_ptr(),
                1,
                &mut error,
            )
        })
    }

    async fn link(
        &self,
        id: fileid3,
        dirid: fileid3,
        filename: &filename3,
    ) -> Result<fattr3, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if state.sidecars.contains_key(&id)
            || RuntimeState::target_name(filename.as_ref()).is_some()
        {
            return Err(nfsstat3::NFS3ERR_NOTSUPP);
        }
        let source_path = state.path_for_inode(id)?;
        let destination_path = state.child_path(dirid, filename.as_ref())?;
        let source = checked_cstring(&source_path)?;
        let destination = checked_cstring(&destination_path)?;
        let mut error = empty_error();
        runtime_result(unsafe {
            vexfs_mount_link(
                state.session,
                source.as_ptr(),
                destination.as_ptr(),
                &mut error,
            )
        })?;
        Ok(stat_attr(&state.stat_path(&source_path)?))
    }

    async fn commit(&self, id: fileid3, _offset: u64, _count: u32) -> Result<fattr3, nfsstat3> {
        let request = self.request_id("nfs-commit");
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        if state.sidecars.contains_key(&id) {
            state.persist_sidecar(id)?;
            return state.sidecar_attr(id);
        }
        let mut published = 0;
        let mut error = empty_error();
        runtime_result(unsafe {
            vexfs_mount_synchronize(state.session, request.as_ptr(), &mut published, &mut error)
        })?;
        let path = state.path_for_inode(id)?;
        Ok(stat_attr(&state.stat_path(&path)?))
    }

    async fn readdir(
        &self,
        dirid: fileid3,
        start_after: fileid3,
        max_entries: usize,
    ) -> Result<ReadDirResult, nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let path = state.path_for_inode(dirid)?;
        let mut entries = state.list(&path)?;
        entries.sort_by_key(|entry| entry.inode);
        let start = if start_after == 0 {
            0
        } else {
            entries
                .iter()
                .position(|entry| entry.inode as u64 == start_after)
                .map(|value| value + 1)
                .unwrap_or(0)
        };
        let end = start.saturating_add(max_entries).min(entries.len());
        let complete = end == entries.len();
        let entries = entries[start..end]
            .iter()
            .map(|entry| {
                let stat: FileStat = entry.clone().into();
                DirEntry {
                    fileid: entry.inode as u64,
                    name: entry.name.as_bytes().into(),
                    attr: stat_attr(&stat),
                }
            })
            .collect();
        Ok(ReadDirResult {
            entries,
            end: complete,
        })
    }

    async fn symlink(
        &self,
        dirid: fileid3,
        linkname: &filename3,
        target: &nfspath3,
        _attrs: &sattr3,
    ) -> Result<(fileid3, fattr3), nfsstat3> {
        let mut state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let path = checked_cstring(&state.child_path(dirid, linkname.as_ref())?)?;
        let mut error = empty_error();
        runtime_result(unsafe {
            vexfs_mount_symlink(
                state.session,
                path.as_ptr(),
                target.as_ref().as_ptr().cast(),
                target.as_ref().len() as u64,
                &mut error,
            )
        })?;
        let path = path.to_str().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let stat = state.stat_path(path)?;
        Ok((stat.inode as u64, stat_attr(&stat)))
    }

    async fn readlink(&self, id: fileid3) -> Result<nfspath3, nfsstat3> {
        let state = self.state.lock().map_err(|_| nfsstat3::NFS3ERR_IO)?;
        let mut bytes = RuntimeBytes {
            data: ptr::null_mut(),
            size: 0,
        };
        let mut error = empty_error();
        let status =
            unsafe { vexfs_mount_readlink(state.session, id as i64, &mut bytes, &mut error) };
        runtime_result(status)?;
        Ok(take_bytes(bytes).into())
    }
}

fn config_string(pointer: *const c_char, name: &str) -> Result<String, String> {
    if pointer.is_null() {
        return Err(format!("{name} is required"));
    }
    unsafe { CStr::from_ptr(pointer) }
        .to_str()
        .map(str::to_owned)
        .map_err(|_| format!("{name} is not UTF-8"))
}

fn write_error(buffer: *mut c_char, size: usize, message: &str) {
    if buffer.is_null() || size == 0 {
        return;
    }
    let bytes = message.as_bytes();
    let count = bytes.len().min(size - 1);
    unsafe {
        ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.cast::<u8>(), count);
        *buffer.add(count) = 0;
    }
}

fn runtime_error_message(error: &RuntimeError) -> String {
    unsafe { CStr::from_ptr(error.message.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}

fn run_gateway(config: &VexfsNfsGatewayConfig) -> Result<(), String> {
    if config.abi_version != RUNTIME_ABI_VERSION {
        return Err("NFS gateway ABI mismatch".to_string());
    }
    let backend = config_string(config.backend, "backend")?;
    let connection = config_string(config.connection, "connection")?;
    let workspace = config_string(config.workspace, "workspace")?;
    let principal = config_string(config.principal, "principal")?;
    let listen = config_string(config.listen_address, "listen address")?;
    if listen != "127.0.0.1" {
        return Err("NFS gateway may only bind 127.0.0.1".to_string());
    }
    if config.port == 0 {
        return Err("NFS gateway port is required".to_string());
    }
    let backend_c = CString::new(backend).map_err(|_| "backend contains NUL")?;
    let connection_c = CString::new(connection).map_err(|_| "connection contains NUL")?;
    let workspace_c = CString::new(workspace).map_err(|_| "workspace contains NUL")?;
    let principal_c = CString::new(principal).map_err(|_| "principal contains NUL")?;
    let runtime_config = RuntimeConfig {
        abi_version: RUNTIME_ABI_VERSION,
        backend: backend_c.as_ptr(),
        connection: connection_c.as_ptr(),
        workspace: workspace_c.as_ptr(),
        principal: principal_c.as_ptr(),
        operation_timeout_ms: config.operation_timeout_ms.max(1),
        flags: RUNTIME_EXCLUSIVE_GATEWAY,
    };
    let mut error = empty_error();
    let mut session = ptr::null_mut();
    let status = unsafe { vexfs_mount_session_open(&runtime_config, &mut session, &mut error) };
    if status != 0 {
        let message = runtime_error_message(&error);
        return Err(if message.is_empty() {
            format!("runtime session open failed with status {status}")
        } else {
            message
        });
    }
    let mut state = RuntimeState {
        session,
        root_inode: 1,
        uid: unsafe { libc::getuid() },
        gid: unsafe { libc::getgid() },
        next_sidecar_inode: SIDECAR_INODE_START,
        sidecars_by_key: HashMap::new(),
        sidecars: HashMap::new(),
    };
    let root = state
        .stat_path("/")
        .map_err(|status| format!("root stat failed: {status:?}"))?;
    state.root_inode = root.inode as u64;
    let prefix = format!("nfs-{}-{}", unsafe { libc::getpid() }, now_ms());
    let filesystem = VexfsNfs {
        state: Mutex::new(state),
        request_prefix: prefix,
        request_sequence: AtomicU64::new(1),
    };
    let bind_address = format!("{listen}:{}", config.port);
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
        .map_err(|error| error.to_string())?;
    runtime.block_on(async move {
        let listener = NFSTcpListener::bind(&bind_address, filesystem)
            .await
            .map_err(|error| error.to_string())?;
        #[cfg(unix)]
        {
            use tokio::signal::unix::{signal, SignalKind};
            let mut terminate =
                signal(SignalKind::terminate()).map_err(|error| error.to_string())?;
            let mut interrupt =
                signal(SignalKind::interrupt()).map_err(|error| error.to_string())?;
            loop {
                tokio::select! {
                    result = listener.handle_forever() => {
                        match result {
                            Ok(()) => return Err("NFS listener stopped unexpectedly".to_string()),
                            Err(error) => {
                                // nfsserve 0.11 returns from its whole accept loop for one
                                // transient accept(2) error. A mounted filesystem must stay
                                // alive, so retain the listener and retry instead of turning a
                                // short macOS network interruption into a stale mount.
                                eprintln!("vexfs-nfs-gateway: transient accept error: {error}");
                                tokio::time::sleep(std::time::Duration::from_millis(100)).await;
                            }
                        }
                    },
                    _ = terminate.recv() => return Ok(()),
                    _ = interrupt.recv() => return Ok(()),
                }
            }
        }
        #[cfg(not(unix))]
        listener
            .handle_forever()
            .await
            .map_err(|error| error.to_string())
    })?;
    // Dropping the listener drops the filesystem and closes the runtime session.
    Ok(())
}

/// Runs the NFS gateway until it receives a termination signal.
///
/// # Safety
///
/// `config` must point to a valid `VexfsNfsGatewayConfig` for the duration of
/// this call. When non-null, `error_message` must reference a writable buffer
/// containing at least `error_message_size` bytes.
#[no_mangle]
pub unsafe extern "C" fn vexfs_nfs_gateway_run(
    config: *const VexfsNfsGatewayConfig,
    error_message: *mut c_char,
    error_message_size: usize,
) -> c_int {
    if config.is_null() {
        write_error(
            error_message,
            error_message_size,
            "gateway config is required",
        );
        return 2;
    }
    let result = std::panic::catch_unwind(|| run_gateway(unsafe { &*config }));
    match result {
        Ok(Ok(())) => 0,
        Ok(Err(message)) => {
            write_error(error_message, error_message_size, &message);
            1
        }
        Err(_) => {
            write_error(error_message, error_message_size, "NFS gateway panicked");
            1
        }
    }
}
