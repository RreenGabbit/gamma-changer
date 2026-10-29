fn main() {
    println!("cargo:rerun-if-changed=src/GammaChanger.rc");
    println!("cargo:rerun-if-changed=src/GammaChanger.manifest");
    println!("cargo:rerun-if-changed=src/assets/flashbang.ico");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        embed_resource::compile("src/GammaChanger.rc", embed_resource::NONE);
    }
}
