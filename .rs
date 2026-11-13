use std::{env, path::PathBuf, process::Command};

fn run(mut command: Command, label: &str) {
    let status = command.status().unwrap_or_else(|error| {
        panic!("failed to start {label}: {error}");
    });
    if !status.success() {
        panic!("{label} failed with status {status}");
    }
}

fn main() {
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR is missing"));
    let object = out_dir.join("lionos_boot.o");
    let archive = out_dir.join("liblionos_boot.a");

    let mut nasm = Command::new("nasm");
    nasm.args(["-f", "elf32", "boot/boot.asm", "-o"])
        .arg(&object);
    run(nasm, "NASM boot assembly");

    let mut ar = Command::new("ar");
    ar.arg("crs").arg(&archive).arg(&object);
    run(ar, "boot archive creation");

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=lionos_boot");
    println!("cargo:rerun-if-changed=boot/boot.asm");
    println!("cargo:rerun-if-changed=linker.ld");
}
