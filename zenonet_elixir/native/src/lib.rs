use rustler::prelude::*;
use std::ffi::{CString, CStr};
use std::ptr;
use std::slice;

mod zenonet {
    #![allow(non_upper_case_globals)]
    #![allow(non_camel_case_types)]
    #![allow(dead_code)]
    
    include!(concat!(env!("OUT_DIR"), "/zenonet_bindings.rs"));
}

// ============================================================================
// Opaque resource types
// ============================================================================

mod resources {
    rustler::atoms! {
        ok,
        error,
        nil,
    }
}

// Server resource
rustler::resource!(ServerResource, impl = {
    resource_down = drop_server
});

fn drop_server(resource: ServerResource) {
    unsafe {
        if !resource.ptr.is_null() {
            zenonet::zn_server_destroy(resource.ptr as *mut zenonet::zn_server);
        }
    }
}

// Client resource
rustler::resource!(ClientResource, impl = {
    resource_down = drop_client
});

fn drop_client(resource: ClientResource) {
    unsafe {
        if !resource.ptr.is_null() {
            zenonet::zn_client_free(resource.ptr as *mut zenonet::zn_client);
        }
    }
}

// Connection resource
rustler::resource!(ConnectionResource, impl = {
    resource_down = drop_connection
});

fn drop_connection(resource: ConnectionResource) {
    unsafe {
        if !resource.ptr.is_null() {
            zenonet::zn_conn_free(resource.ptr as *mut zenonet::zn_conn);
        }
    }
}

// ============================================================================
// Helper functions
// ============================================================================

fn c_string_to_binary(c_str: *const i8) -> Binary {
    unsafe {
        let c_str = CStr::from_ptr(c_str);
        let bytes = c_str.to_bytes();
        Binary::from_owned(bytes.to_vec())
    }
}

fn to_c_string(s: &str) -> Result<CString, Error> {
    CString::new(s).map_err(|_| Error::Term(Box::new("Invalid string")))
}

fn encode_term(ptr: *mut std::os::raw::c_void) -> Term {
    unsafe {
        let server = ptr as *mut zenonet::zn_server;
        let count = zenonet::zn_server_conn_count(server);
        let mut conns = vec![];
        
        for i in 0..count {
            let conn = zenonet::zn_server_get_conn(server, 
                std::ptr::null() as *const i8); // This won't work, need to iterate properly
        }
        
        Term::from(conns)
    }
}

// ============================================================================
// Server NIFs
// ============================================================================

#[rustler::nif]
fn server_create(port: u16) -> Result<ResourceArc<ServerResource>, Error> {
    unsafe {
        let server = zenonet::zn_server_create(port);
        if server.is_null() {
            return Err(Error::Term(Box::new("Failed to create server")));
        }
        
        let resource = ServerResource {
            ptr: server as *mut std::os::raw::c_void
        };
        
        Ok(ResourceArc::new(resource))
    }
}

#[rustler::nif]
fn server_create_host(host: String, port: u16) -> Result<ResourceArc<ServerResource>, Error> {
    unsafe {
        let host_c = to_c_string(&host)?;
        let server = zenonet::zn_server_create_host(host_c.as_ptr(), port);
        if server.is_null() {
            return Err(Error::Term(Box::new("Failed to create server")));
        }
        
        let resource = ServerResource {
            ptr: server as *mut std::os::raw::c_void
        };
        
        Ok(ResourceArc::new(resource))
    }
}

#[rustler::nif]
fn server_start(server: ResourceArc<ServerResource>) -> Result<bool, Error> {
    unsafe {
        let srv = server.ptr as *mut zenonet::zn_server;
        Ok(zenonet::zn_server_start(srv))
    }
}

#[rustler::nif]
fn server_stop(server: ResourceArc<ServerResource>) {
    unsafe {
        let srv = server.ptr as *mut zenonet::zn_server;
        zenonet::zn_server_stop(srv);
    }
}

#[rustler::nif]
fn server_broadcast(server: ResourceArc<ServerResource>, data: String) {
    unsafe {
        let srv = server.ptr as *mut zenonet::zn_server;
        let data_c = to_c_string(&data).unwrap();
        zenonet::zn_server_broadcast_data(srv, data_c.as_ptr(), std::ptr::null_mut());
    }
}

#[rustler::nif]
fn server_connection_count(server: ResourceArc<ServerResource>) -> u32 {
    unsafe {
        let srv = server.ptr as *mut zenonet::zn_server;
        zenonet::zn_server_conn_count(srv) as u32
    }
}

#[rustler::nif]
fn server_on_connect(
    server: ResourceArc<ServerResource>,
    pid: Atom,
    function: Atom
) -> Result<(), Error> {
    // Store callback info - in real implementation, use a proper callback mechanism
    // For now, this is a placeholder
    Ok(())
}

#[rustler::nif]
fn server_on_disconnect(
    server: ResourceArc<ServerResource>,
    pid: Atom,
    function: Atom
) -> Result<(), Error> {
    Ok(())
}

#[rustler::nif]
fn server_on_data(
    server: ResourceArc<ServerResource>,
    pid: Atom,
    function: Atom
) -> Result<(), Error> {
    Ok(())
}

// ============================================================================
// Client NIFs
// ============================================================================

#[rustler::nif]
fn client_new() -> Result<ResourceArc<ClientResource>, Error> {
    unsafe {
        let client = zenonet::zn_client_new();
        if client.is_null() {
            return Err(Error::Term(Box::new("Failed to create client")));
        }
        
        let resource = ClientResource {
            ptr: client as *mut std::os::raw::c_void
        };
        
        Ok(ResourceArc::new(resource))
    }
}

#[rustler::nif]
fn client_connect(
    client: ResourceArc<ClientResource>,
    host: String,
    port: u16
) -> Result<bool, Error> {
    unsafe {
        let cli = client.ptr as *mut zenonet::zn_client;
        let host_c = to_c_string(&host)?;
        Ok(zenonet::zn_client_connect(cli, host_c.as_ptr(), port))
    }
}

#[rustler::nif]
fn client_connect_name(
    client: ResourceArc<ClientResource>,
    host: String,
    port: u16,
    name: String
) -> Result<bool, Error> {
    unsafe {
        let cli = client.ptr as *mut zenonet::zn_client;
        let host_c = to_c_string(&host)?;
        let name_c = to_c_string(&name)?;
        Ok(zenonet::zn_client_connect_name(cli, host_c.as_ptr(), port, name_c.as_ptr()))
    }
}

#[rustler::nif]
fn client_disconnect(client: ResourceArc<ClientResource>) {
    unsafe {
        let cli = client.ptr as *mut zenonet::zn_client;
        zenonet::zn_client_disconnect(cli);
    }
}

#[rustler::nif]
fn client_send(client: ResourceArc<ClientResource>, data: String) -> Result<bool, Error> {
    unsafe {
        let cli = client.ptr as *mut zenonet::zn_client;
        let data_c = to_c_string(&data)?;
        Ok(zenonet::zn_client_send_data(cli, data_c.as_ptr()))
    }
}

#[rustler::nif]
fn client_join_room(client: ResourceArc<ClientResource>, room: String) -> Result<bool, Error> {
    unsafe {
        let cli = client.ptr as *mut zenonet::zn_client;
        let room_c = to_c_string(&room)?;
        Ok(zenonet::zn_client_join_room(cli, room_c.as_ptr()))
    }
}

#[rustler::nif]
fn client_leave_room(client: ResourceArc<ClientResource>) -> Result<bool, Error> {
    unsafe {
        let cli = client.ptr as *mut zenonet::zn_client;
        Ok(zenonet::zn_client_leave_room(cli))
    }
}

#[rustler::nif]
fn client_is_connected(client: ResourceArc<ClientResource>) -> bool {
    unsafe {
        let cli = client.ptr as *mut zenonet::zn_client;
        zenonet::zn_client_is_connected(cli)
    }
}

#[rustler::nif]
fn client_on_connect(
    client: ResourceArc<ClientResource>,
    pid: Atom,
    function: Atom
) -> Result<(), Error> {
    Ok(())
}

#[rustler::nif]
fn client_on_disconnect(
    client: ResourceArc<ClientResource>,
    pid: Atom,
    function: Atom
) -> Result<(), Error> {
    Ok(())
}

#[rustler::nif]
fn client_on_data(
    client: ResourceArc<ClientResource>,
    pid: Atom,
    function: Atom
) -> Result<(), Error> {
    Ok(())
}

// ============================================================================
// Packet NIFs
// ============================================================================

#[rustler::nif]
fn packet_create(packet_type: u8, data: String) -> Result<Binary, Error> {
    unsafe {
        let pkt = zenonet::zn_packet_create(packet_type as u32, to_c_string(&data)?.as_ptr());
        if pkt.is_null() {
            return Err(Error::Term(Box::new("Failed to create packet")));
        }
        
        let mut len = 0;
        let serialized = zenonet::zn_packet_serialize(pkt, &mut len);
        zenonet::zn_packet_free(pkt);
        
        if serialized.is_null() || len == 0 {
            return Err(Error::Term(Box::new("Failed to serialize packet")));
        }
        
        let bytes = slice::from_raw_parts(serialized, len as usize);
        let binary = Binary::from_owned(bytes.to_vec());
        
        // Free the serialized data
        libc::free(serialized as *mut libc::c_void);
        
        Ok(binary)
    }
}

#[rustler::nif]
fn packet_create_with_room(
    packet_type: u8,
    data: String,
    room: String
) -> Result<Binary, Error> {
    unsafe {
        let data_c = to_c_string(&data)?;
        let room_c = to_c_string(&room)?;
        let pkt = zenonet::zn_packet_create_with_room(
            packet_type as u32,
            data_c.as_ptr(),
            room_c.as_ptr()
        );
        
        if pkt.is_null() {
            return Err(Error::Term(Box::new("Failed to create packet")));
        }
        
        let mut len = 0;
        let serialized = zenonet::zn_packet_serialize(pkt, &mut len);
        zenonet::zn_packet_free(pkt);
        
        if serialized.is_null() || len == 0 {
            return Err(Error::Term(Box::new("Failed to serialize packet")));
        }
        
        let bytes = slice::from_raw_parts(serialized, len as usize);
        let binary = Binary::from_owned(bytes.to_vec());
        
        libc::free(serialized as *mut libc::c_void);
        
        Ok(binary)
    }
}

// ============================================================================
// Utility NIFs
// ============================================================================

#[rustler::nif]
fn version() -> String {
    unsafe {
        let version = zenonet::zn_version_string();
        c_string_to_binary(version).to_string()
    }
}

#[rustler::nif]
fn timestamp() -> u32 {
    unsafe {
        zenonet::zn_timestamp()
    }
}

// ============================================================================
// NIF initialization
// ============================================================================

rustler::init!("Elixir.ZenoNET.Native", [
    // Server functions
    server_create,
    server_create_host,
    server_start,
    server_stop,
    server_broadcast,
    server_connection_count,
    server_on_connect,
    server_on_disconnect,
    server_on_data,
    
    // Client functions
    client_new,
    client_connect,
    client_connect_name,
    client_disconnect,
    client_send,
    client_join_room,
    client_leave_room,
    client_is_connected,
    client_on_connect,
    client_on_disconnect,
    client_on_data,
    
    // Packet functions
    packet_create,
    packet_create_with_room,
    
    // Utility functions
    version,
    timestamp,
]);