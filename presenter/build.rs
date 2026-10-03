fn main() {
    // gbm_create_device(), the one libgbm call (see src/gpu.rs).
    println!("cargo:rustc-link-lib=gbm");
}
