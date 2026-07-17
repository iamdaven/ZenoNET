fn main() {
    // Build the C library
    let mut build = cc::Build::new();
    
    build
        .include("../../include")
        .include("../../src")
        .file("../../src/zenonet.c")
        .file("../../src/packet.c")
        .file("../../src/connection.c")
        .file("../../src/room.c")
        .file("../../src/server.c")
        .file("../../src/client.c");
    
    if cfg!(target_os = "windows") {
        build.define("_WIN32", "1");
    }
    
    build.compile("zenonet_c");
    
    // Tell cargo to link the library
    println!("cargo:rustc-link-lib=static=zenonet_c");
    
    // On Windows, we need to link ws2_32
    if cfg!(target_os = "windows") {
        println!("cargo:rustc-link-lib=ws2_32");
    }
    
    // Generate NIF module
    rustler_codegen::init!("Elixir.ZenoNET.Native");
}