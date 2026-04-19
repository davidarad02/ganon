-- GNN Protocol Dissector for Wireshark
-- Place this file in ~/.config/wireshark/plugins/ (Linux)
-- or %APPDATA%\Wireshark\plugins\ (Windows)
-- or ~/Library/Wireshark/plugins/ (macOS)

local gnw_proto = Proto("GNN", "Ganon Mesh Network Protocol")

-- Protocol fields
local fields = gnw_proto.fields

fields.magic = ProtoField.string("gnn.magic", "Magic", base.ASCII)
fields.orig_src_node_id = ProtoField.uint32("gnn.orig_src_node_id", "Original Source Node ID", base.DEC)
fields.src_node_id = ProtoField.uint32("gnn.src_node_id", "Source Node ID", base.DEC)
fields.dst_node_id = ProtoField.uint32("gnn.dst_node_id", "Destination Node ID", base.DEC)
fields.message_id = ProtoField.uint32("gnn.message_id", "Message ID", base.DEC)
fields.message_type = ProtoField.uint32("gnn.message_type", "Message Type", base.DEC)
fields.data_length = ProtoField.uint32("gnn.data_length", "Data Length", base.DEC)
fields.ttl = ProtoField.uint32("gnn.ttl", "TTL", base.DEC)

-- Message type names
local msg_type_names = {
    [0] = "NODE_INIT",
    [1] = "PEER_INFO",
    [2] = "NODE_DISCONNECT",
    [3] = "CONNECTION_REJECTED"
}

-- Dissector function
function gnw_proto.dissector(buffer, pinfo, tree)
    -- Minimum header size is 32 bytes (8 x 4-byte fields)
    local MIN_HEADER_SIZE = 32
    
    -- Check if packet is long enough
    if buffer:len() < MIN_HEADER_SIZE then
        return
    end
    
    -- Check for GNN magic bytes
    local magic = buffer(0, 4):string()
    if magic ~= "GNN\0" and magic ~= "GNN" then
        return
    end
    
    -- Set protocol name in packet list
    pinfo.cols.protocol = gnw_proto.name
    
    -- Create protocol tree
    local subtree = tree:add(gnw_proto, buffer(), "GNN Protocol")
    
    -- Add magic
    subtree:add(fields.magic, buffer(0, 4))
    
    -- Get all header values (network byte order = big endian)
    local orig_src = buffer(4, 4):uint()
    local src = buffer(8, 4):uint()
    local dst = buffer(12, 4):uint()
    local msg_id = buffer(16, 4):uint()
    local msg_type = buffer(20, 4):uint()
    local data_len = buffer(24, 4):uint()
    local ttl = buffer(28, 4):uint()
    
    -- Add header fields
    subtree:add(fields.orig_src_node_id, buffer(4, 4)):set_text("Original Source Node ID: " .. orig_src)
    subtree:add(fields.src_node_id, buffer(8, 4)):set_text("Source Node ID: " .. src)
    subtree:add(fields.dst_node_id, buffer(12, 4)):set_text("Destination Node ID: " .. dst)
    subtree:add(fields.message_id, buffer(16, 4)):set_text("Message ID: " .. msg_id)
    
    -- Add message type with name
    local type_tree = subtree:add(fields.message_type, buffer(20, 4))
    local type_name = msg_type_names[msg_type] or "UNKNOWN"
    type_tree:set_text("Message Type: " .. msg_type .. " (" .. type_name .. ")")
    
    subtree:add(fields.data_length, buffer(24, 4)):set_text("Data Length: " .. data_len .. " bytes")
    subtree:add(fields.ttl, buffer(28, 4)):set_text("TTL: " .. ttl)
    
    -- Add info column
    pinfo.cols.info:set(string.format("%s: orig=%u, src=%u, dst=%u, ttl=%u", 
        type_name, orig_src, src, dst, ttl))
    
    -- Add data section if present
    if data_len > 0 and buffer:len() >= MIN_HEADER_SIZE + data_len then
        local data_subtree = subtree:add(buffer(MIN_HEADER_SIZE, data_len), "Data (" .. data_len .. " bytes)")
        
        -- For PEER_INFO, parse the peer list
        if msg_type == 1 then
            local peer_count = data_len / 4
            local offset = MIN_HEADER_SIZE
            local peers = {}
            for i = 1, peer_count do
                local peer_id = buffer(offset, 4):uint()
                peers[i] = peer_id
                offset = offset + 4
            end
            data_subtree:set_text("Peer List: " .. table.concat(peers, ", "))
            
            for i = 1, peer_count do
                data_subtree:add(buffer(MIN_HEADER_SIZE + (i-1) * 4, 4), 
                    string.format("Peer[%d]: Node ID %u", i-1, peers[i]))
            end
        end
    end
    
    return buffer:len()
end

-- Register dissector to TCP port 5555 (default GNN port)
local tcp_table = DissectorTable.get("tcp.port")
tcp_table:add(5555, gnw_proto)

print("GNN Protocol dissector loaded successfully")
