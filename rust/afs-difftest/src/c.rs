use std::cell::RefCell;
use std::ffi::{CStr, c_char, c_int, c_void};

use afs::{Config, lookup_table_len};

use crate::sim::SimStorage;

#[repr(C, align(8))]
pub struct AfsHandleDef([u8; 120]);

#[repr(C, align(8))]
pub struct AfsObjectHandleDef([u8; 264]);

#[repr(C, align(8))]
#[derive(Clone, Copy)]
pub struct ReadPositionC(pub [u8; 208]);

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ObjectListEntryC {
    private: [u8; 4],
    pub object_id: u16,
}

#[repr(C)]
struct StorageConfigC {
    block_size: u32,
    num_blocks: u16,
    sub_blocks_per_block: u32,
    min_read_write_size: u32,
    read: extern "C" fn(*mut u8, u16, u32, u32),
    write: extern "C" fn(*const u8, u16, u32, u32),
    erase: extern "C" fn(u16),
}

#[repr(C)]
struct InitC {
    storage_config: StorageConfigC,
    read_write_buffer: *mut u8,
    lookup_table_buffer: *mut c_void,
    object_found: Option<extern "C" fn(u16, u8, *const u8, u32)>,
}

#[repr(C)]
struct ObjectConfigC {
    buffer: *mut u8,
    buffer_size: u32,
}

unsafe extern "C" {
    fn afs_init(afs: *mut AfsHandleDef, init: *const InitC);
    fn afs_deinit(afs: *mut AfsHandleDef);
    fn afs_object_create(afs: *mut AfsHandleDef, obj: *mut AfsObjectHandleDef, config: *const ObjectConfigC) -> u16;
    fn afs_object_write(
        afs: *mut AfsHandleDef,
        obj: *mut AfsObjectHandleDef,
        stream: u8,
        data: *const u8,
        length: u32,
    ) -> bool;
    fn afs_object_open(
        afs: *mut AfsHandleDef,
        obj: *mut AfsObjectHandleDef,
        stream: u8,
        object_id: u16,
        config: *const ObjectConfigC,
    ) -> bool;
    fn afs_object_read(
        afs: *mut AfsHandleDef,
        obj: *mut AfsObjectHandleDef,
        data: *mut u8,
        max_length: u32,
        stream: *mut u8,
    ) -> u32;
    fn afs_object_seek(afs: *mut AfsHandleDef, obj: *mut AfsObjectHandleDef, offset: u64) -> bool;
    fn afs_object_size(afs: *mut AfsHandleDef, obj: *mut AfsObjectHandleDef, bitmask: u16) -> u64;
    fn afs_object_save_read_position(afs: *mut AfsHandleDef, obj: *mut AfsObjectHandleDef, pos: *mut ReadPositionC);
    fn afs_object_restore_read_position(afs: *mut AfsHandleDef, obj: *mut AfsObjectHandleDef, pos: *mut ReadPositionC);
    fn afs_object_close(afs: *mut AfsHandleDef, obj: *mut AfsObjectHandleDef) -> bool;
    fn afs_object_list(afs: *mut AfsHandleDef, entry: *mut ObjectListEntryC) -> bool;
    fn afs_object_get_num_blocks(afs: *mut AfsHandleDef, object_id: u16) -> u16;
    fn afs_object_delete(afs: *mut AfsHandleDef, object_id: u16);
    fn afs_wipe(afs: *mut AfsHandleDef, secure: bool);
    fn afs_size(afs: *mut AfsHandleDef) -> u16;
    fn afs_is_storage_full(afs: *mut AfsHandleDef) -> bool;
    fn afs_prepare_storage(afs: *mut AfsHandleDef, num_blocks: u16);
    fn afs_dump(afs: *mut AfsHandleDef);
    fn afs_dump_block(afs: *mut AfsHandleDef, block: u16, max_chunks: u32);
    fn afs_dump_object(afs: *mut AfsHandleDef, object_id: u16);
    fn afs_difftest_sizeof(which: c_int) -> usize;
    fn afs_difftest_get_seed(afs: *mut AfsHandleDef) -> u32;
    fn afs_difftest_set_seed(afs: *mut AfsHandleDef, seed: u32);
}

thread_local! {
    static STORAGE: RefCell<Option<SimStorage>> = const { RefCell::new(None) };
    static LOG: RefCell<Option<Vec<String>>> = const { RefCell::new(None) };
    static FOUND: RefCell<Vec<(u16, u8, Vec<u8>)>> = const { RefCell::new(Vec::new()) };
}

fn with_storage(f: impl FnOnce(&SimStorage)) {
    STORAGE.with_borrow(|storage| f(storage.as_ref().expect("no C storage attached")));
}

extern "C" fn read_cb(buf: *mut u8, block: u16, offset: u32, length: u32) {
    let buf = unsafe { std::slice::from_raw_parts_mut(buf, length as usize) };
    with_storage(|s| s.c_read(block, offset, buf));
}

extern "C" fn write_cb(buf: *const u8, block: u16, offset: u32, length: u32) {
    let data = if length == 0 { &[][..] } else { unsafe { std::slice::from_raw_parts(buf, length as usize) } };
    with_storage(|s| s.c_write(block, offset, data));
}

extern "C" fn erase_cb(block: u16) {
    with_storage(|s| s.c_erase(block));
}

extern "C" fn found_cb(object_id: u16, stream: u8, data: *const u8, length: u32) {
    let data =
        if length == 0 { Vec::new() } else { unsafe { std::slice::from_raw_parts(data, length as usize) }.to_vec() };
    FOUND.with_borrow_mut(|found| found.push((object_id, stream, data)));
}

#[unsafe(no_mangle)]
extern "C" fn afs_difftest_log_sink(_level: c_int, message: *const c_char) {
    let message = unsafe { CStr::from_ptr(message) }.to_string_lossy().into_owned();
    if std::env::var_os("AFS_TRACE_OPS").is_some() {
        eprintln!("C: {message}");
    }
    LOG.with_borrow_mut(|log| {
        if let Some(log) = log {
            log.push(message);
        }
    });
}

fn check_layout() {
    let sizes = unsafe { [0, 1, 2, 3, 4, 5, 6].map(|i| afs_difftest_sizeof(i)) };
    assert_eq!(
        sizes,
        [
            size_of::<AfsHandleDef>(),
            size_of::<AfsObjectHandleDef>(),
            size_of::<ReadPositionC>(),
            size_of::<ObjectListEntryC>(),
            size_of::<InitC>(),
            size_of::<ObjectConfigC>(),
            std::mem::offset_of!(InitC, lookup_table_buffer),
        ]
    );
}

pub struct CObject {
    handle: Box<AfsObjectHandleDef>,
    buffer: Vec<u8>,
}

impl CObject {
    fn new(buffer_size: usize) -> Self {
        Self { handle: Box::new(AfsObjectHandleDef([0; 264])), buffer: vec![0; buffer_size] }
    }

    fn config(&mut self) -> ObjectConfigC {
        ObjectConfigC { buffer: self.buffer.as_mut_ptr(), buffer_size: self.buffer.len() as u32 }
    }
}

pub struct CAfs {
    handle: Box<AfsHandleDef>,
    _lookup: Vec<u32>,
    _rw: Vec<u8>,
    pub storage: SimStorage,
}

pub type Found = Vec<(u16, u8, Vec<u8>)>;

impl CAfs {
    pub fn mount(storage: SimStorage, with_callback: bool) -> (Self, Found) {
        check_layout();
        let config: Config = storage.inner().config;
        let mut lookup = vec![0u32; lookup_table_len(config.num_blocks)];
        let mut rw = vec![0u8; config.min_read_write_size as usize];
        let mut handle = Box::new(AfsHandleDef([0; 120]));
        STORAGE.with_borrow_mut(|s| *s = Some(storage.clone()));
        FOUND.with_borrow_mut(Vec::clear);
        let init = InitC {
            storage_config: StorageConfigC {
                block_size: config.block_size,
                num_blocks: config.num_blocks,
                sub_blocks_per_block: config.sub_blocks_per_block,
                min_read_write_size: config.min_read_write_size,
                read: read_cb,
                write: write_cb,
                erase: erase_cb,
            },
            read_write_buffer: rw.as_mut_ptr(),
            lookup_table_buffer: lookup.as_mut_ptr().cast(),
            object_found: with_callback.then_some(found_cb as extern "C" fn(u16, u8, *const u8, u32)),
        };
        unsafe { afs_init(&mut *handle, &init) };
        let found = FOUND.with_borrow_mut(std::mem::take);
        (Self { handle, _lookup: lookup, _rw: rw, storage }, found)
    }

    fn h(&mut self) -> *mut AfsHandleDef {
        STORAGE.with_borrow_mut(|s| *s = Some(self.storage.clone()));
        &mut *self.handle
    }

    pub fn deinit(mut self) {
        unsafe { afs_deinit(self.h()) };
    }

    pub fn seed(&mut self) -> u32 {
        unsafe { afs_difftest_get_seed(self.h()) }
    }

    pub fn set_seed(&mut self, seed: u32) {
        unsafe { afs_difftest_set_seed(self.h(), seed) }
    }

    pub fn create(&mut self, buffer_size: usize) -> (u16, CObject) {
        let mut obj = CObject::new(buffer_size);
        let config = obj.config();
        let id = unsafe { afs_object_create(self.h(), &mut *obj.handle, &config) };
        (id, obj)
    }

    pub fn write(&mut self, obj: &mut CObject, stream: u8, data: &[u8]) -> bool {
        unsafe { afs_object_write(self.h(), &mut *obj.handle, stream, data.as_ptr(), data.len() as u32) }
    }

    pub fn open(&mut self, object_id: u16, stream: u8, buffer_size: usize) -> Option<CObject> {
        let mut obj = CObject::new(buffer_size);
        let config = obj.config();
        unsafe { afs_object_open(self.h(), &mut *obj.handle, stream, object_id, &config) }.then_some(obj)
    }

    pub fn read(&mut self, obj: &mut CObject, buf: &mut [u8]) -> usize {
        let length = unsafe {
            afs_object_read(self.h(), &mut *obj.handle, buf.as_mut_ptr(), buf.len() as u32, std::ptr::null_mut())
        };
        length as usize
    }

    pub fn read_chunk(&mut self, obj: &mut CObject, buf: &mut [u8]) -> Option<(u8, usize)> {
        let mut stream = u8::MAX;
        let length =
            unsafe { afs_object_read(self.h(), &mut *obj.handle, buf.as_mut_ptr(), buf.len() as u32, &mut stream) };
        (length > 0).then_some((stream, length as usize))
    }

    pub fn seek(&mut self, obj: &mut CObject, offset: u64) -> bool {
        unsafe { afs_object_seek(self.h(), &mut *obj.handle, offset) }
    }

    pub fn size(&mut self, obj: &mut CObject, bitmask: u16) -> u64 {
        unsafe { afs_object_size(self.h(), &mut *obj.handle, bitmask) }
    }

    pub fn save_position(&mut self, obj: &mut CObject) -> ReadPositionC {
        let mut pos = ReadPositionC([0; 208]);
        unsafe { afs_object_save_read_position(self.h(), &mut *obj.handle, &mut pos) };
        pos
    }

    pub fn restore_position(&mut self, obj: &mut CObject, mut pos: ReadPositionC) {
        unsafe { afs_object_restore_read_position(self.h(), &mut *obj.handle, &mut pos) };
    }

    pub fn close(&mut self, obj: &mut CObject) -> bool {
        unsafe { afs_object_close(self.h(), &mut *obj.handle) }
    }

    pub fn list_next(&mut self, entry: &mut ObjectListEntryC) -> Option<u16> {
        unsafe { afs_object_list(self.h(), entry) }.then_some(entry.object_id)
    }

    pub fn list(&mut self) -> Vec<u16> {
        let mut entry = ObjectListEntryC::default();
        std::iter::from_fn(|| self.list_next(&mut entry)).collect()
    }

    pub fn num_blocks(&mut self, object_id: u16) -> u16 {
        unsafe { afs_object_get_num_blocks(self.h(), object_id) }
    }

    pub fn delete(&mut self, object_id: u16) {
        unsafe { afs_object_delete(self.h(), object_id) }
    }

    pub fn wipe(&mut self, secure: bool) {
        unsafe { afs_wipe(self.h(), secure) }
    }

    pub fn size_blocks(&mut self) -> u16 {
        unsafe { afs_size(self.h()) }
    }

    pub fn is_full(&mut self) -> bool {
        unsafe { afs_is_storage_full(self.h()) }
    }

    pub fn prepare_storage(&mut self, num_blocks: u16) {
        unsafe { afs_prepare_storage(self.h(), num_blocks) }
    }

    fn capture(&mut self, f: impl FnOnce(*mut AfsHandleDef)) -> String {
        let h = self.h();
        LOG.with_borrow_mut(|log| *log = Some(Vec::new()));
        f(h);
        let lines = LOG.with_borrow_mut(Option::take).unwrap_or_default();
        lines.iter().map(|line| format!("{line}\n")).collect()
    }

    pub fn dump(&mut self) -> String {
        self.capture(|h| unsafe { afs_dump(h) })
    }

    pub fn dump_block(&mut self, block: u16, max_chunks: u32) -> String {
        self.capture(|h| unsafe { afs_dump_block(h, block, max_chunks) })
    }

    pub fn dump_object(&mut self, object_id: u16) -> String {
        self.capture(|h| unsafe { afs_dump_object(h, object_id) })
    }
}
