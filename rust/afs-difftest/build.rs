use std::path::PathBuf;

const SOURCES: &[&str] = &[
    "afs.c",
    "afs_debug.c",
    "cache.c",
    "compile_checks.c",
    "lookup_table.c",
    "object_read.c",
    "object_seek.c",
    "object_write.c",
    "open_object_list.c",
    "storage.c",
    "util.c",
];

fn main() {
    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let afs_root = manifest_dir.join("../..");
    let src = afs_root.join("src");
    let mut build = cc::Build::new();
    build
        .include(manifest_dir.join("c"))
        .include(afs_root.join("include"))
        .include(&src)
        .file(manifest_dir.join("c/shim.c"))
        .flag_if_supported("-Wno-unused-variable")
        .flag_if_supported("-Wno-unused-but-set-variable")
        .flag_if_supported("-Wno-unused-parameter");
    for source in SOURCES {
        build.file(src.join(source));
    }
    build.compile("afs_c");
    println!("cargo:rerun-if-changed=c");
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed={}", afs_root.join("include").display());
}
