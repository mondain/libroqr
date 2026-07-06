// Locates libroqr-ffi.so and wires up linking for the example.
//
// The directory holding libroqr-ffi.so is taken from the ROQR_FFI_LIB_DIR
// environment variable, defaulting to the project's `dev` CMake build output
// (build/dev/ffi). An rpath is baked in so the binary runs without setting
// LD_LIBRARY_PATH.

use std::path::PathBuf;

fn main() {
    let manifest = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());

    let lib_dir = match std::env::var("ROQR_FFI_LIB_DIR") {
        Ok(dir) => PathBuf::from(dir),
        // examples/rust -> ../../build/dev/ffi
        Err(_) => manifest.join("../../build/dev/ffi"),
    };
    let lib_dir = lib_dir.canonicalize().unwrap_or(lib_dir);

    if !lib_dir.join("libroqr-ffi.so").exists() {
        println!(
            "cargo:warning=libroqr-ffi.so not found in {}. Build the FFI library \
             (e.g. `cmake --preset dev && cmake --build --preset dev`) or set \
             ROQR_FFI_LIB_DIR to the directory that contains it.",
            lib_dir.display()
        );
    }

    println!("cargo:rustc-link-search=native={}", lib_dir.display());
    println!("cargo:rustc-link-lib=dylib=roqr-ffi");
    // Bake an rpath so the example runs without LD_LIBRARY_PATH.
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
    println!("cargo:rerun-if-env-changed=ROQR_FFI_LIB_DIR");
}
