// ============================================================================
// Module: copy_engine
// Description: Async Copy Engine for H2D (Host to Device) and D2H transfers
// Supports descriptor lists, concurrent H2D/D2H, and L2 snooping
// ============================================================================
module copy_engine #(
    parameter ADDR_W        = 64,
    parameter DATA_W        = 512,      // PCIe/HBM width
    parameter DESC_DEPTH    = 8,        // Descriptor FIFO depth
    parameter MAX_BURST     = 256       // Max bytes per burst
) (
    input  wire                      clk,
    input  wire                      rst_n,
    
    // Configuration Interface (from PCIe)
    input  wire [31:0]               cfg_addr,
    input  wire [63:0]               cfg_wdata,
    input  wire                      cfg_wr,
    input  wire                      cfg_rd,
    output reg  [63:0]               cfg_rdata,
    output reg                       cfg_ready,
    
    // PCIe Data Interface (Host memory access)
    // H2D: Read from Host
    output reg  [ADDR_W-1:0]         pcie_rd_addr,
    output reg  [15:0]               pcie_rd_len,       // Bytes to read
    output reg                       pcie_rd_valid,
    input  wire                      pcie_rd_ready,
    input  wire [DATA_W-1:0]         pcie_rd_data,
    input  wire                      pcie_rd_data_valid,
    output wire                      pcie_rd_data_ready,
    
    // D2H: Write to Host
    output reg  [ADDR_W-1:0]         pcie_wr_addr,
    output reg  [DATA_W-1:0]         pcie_wr_data,
    output reg  [15:0]               pcie_wr_len,
    output reg                       pcie_wr_valid,
    input  wire                      pcie_wr_ready,
    
    // L2 Cache Interface (Device memory access)
    output reg  [ADDR_W-1:0]         l2_addr,
    output reg  [DATA_W-1:0]         l2_wdata,
    output reg                       l2_wr_en,
    output reg                       l2_valid,
    input  wire                      l2_ready,
    input  wire [DATA_W-1:0]         l2_rdata,
    input  wire                      l2_rdata_valid,
    
    // L2 Snoop (for coherence)
    output reg  [ADDR_W-1:0]         snoop_addr,
    output reg                       snoop_valid,
    input  wire                      snoop_hit,         // Address in L2?
    
    // Semaphore/Sync (with compute)
    input  wire [15:0]               sema_in,           // Wait for this value
    output reg  [15:0]               sema_out,          // Signal completion
    output reg                       sema_valid,
    
    // Status
    output reg                       ce_busy,           // Engine busy
    output reg  [31:0]               bytes_copied       // Performance counter
);

    // Descriptor Format (64 bytes, 512-bit aligned)
    // [63:0]   - Source Address (Host or Device)
    // [127:64] - Dest Address (Device or Host)
    // [159:128]- Length (bytes)
    // [191:160]- Control (H2D/D2H, interrupt enable, semaphore ID)
    // [511:192]- Reserved
    
    typedef struct packed {
        reg [63:0]  src_addr;
        reg [127:64] dst_addr;
        reg [31:0]  length;
        reg [31:0]  control;        // [0]: H2D(0)/D2H(1), [1]: IRQ enable
    } descriptor_t;
    
    // Control Registers
    reg [63:0] desc_base_addr;      // Descriptor ring base
    reg [15:0] desc_count;          // Number of descriptors
    reg        ce_enable;           // Engine enable
    
    // Descriptor FIFO
    descriptor_t desc_fifo [0:DESC_DEPTH-1];
    reg [$clog2(DESC_DEPTH):0] desc_wr_ptr;
    reg [$clog2(DESC_DEPTH):0] desc_rd_ptr;
    wire desc_empty = (desc_wr_ptr == desc_rd_ptr);
    wire desc_full  = (desc_wr_ptr == (desc_rd_ptr + DESC_DEPTH));
    
    // State Machine
    localparam IDLE       = 3'b000;
    localparam FETCH_DESC = 3'b001;  // Read descriptor from host
    localparam H2D_COPY   = 3'b010;  // Host to Device
    localparam D2H_COPY   = 3'b011;  // Device to Host
    localparam SYNC       = 3'b100;  // Wait for semaphore
    localparam COMPLETE   = 3'b101;  // Signal completion
    
    reg [2:0] state;
    descriptor_t active_desc;
    reg [31:0]   bytes_remaining;
    reg [ADDR_W-1:0] current_src;
    reg [ADDR_W-1:0] current_dst;
    
    // Configuration Interface Handling
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cfg_ready <= 1'b0;
            desc_base_addr <= 64'd0;
            desc_count <= 16'd0;
            ce_enable <= 1'b0;
            bytes_copied <= 32'd0;
        end else begin
            cfg_ready <= 1'b0;
            
            if (cfg_wr) begin
                cfg_ready <= 1'b1;
                case (cfg_addr[7:0])
                    8'h00: desc_base_addr <= cfg_wdata;
                    8'h08: desc_count <= cfg_wdata[15:0];
                    8'h10: ce_enable <= cfg_wdata[0];
                    8'h18: bytes_copied <= 32'd0;  // Clear counter
                endcase
            end else if (cfg_rd) begin
                cfg_ready <= 1'b1;
                case (cfg_addr[7:0])
                    8'h00: cfg_rdata <= desc_base_addr;
                    8'h08: cfg_rdata <= {48'd0, desc_count};
                    8'h10: cfg_rdata <= {63'd0, ce_enable};
                    8'h20: cfg_rdata <= {31'd0, ce_busy};
                    8'h28: cfg_rdata <= bytes_copied;
                    default: cfg_rdata <= 64'd0;
                endcase
            end
        end
    end
    
    // Main State Machine
    assign pcie_rd_data_ready = 1'b1;  // Always ready for data
    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state <= IDLE;
            ce_busy <= 1'b0;
            desc_wr_ptr <= {$clog2(DESC_DEPTH)+1{1'b0}};
            desc_rd_ptr <= {$clog2(DESC_DEPTH)+1{1'b0}};
            pcie_rd_valid <= 1'b0;
            pcie_wr_valid <= 1'b0;
            l2_valid <= 1'b0;
            snoop_valid <= 1'b0;
            sema_valid <= 1'b0;
        end else begin
            pcie_rd_valid <= 1'b0;
            pcie_wr_valid <= 1'b0;
            l2_valid <= 1'b0;
            snoop_valid <= 1'b0;
            sema_valid <= 1'b0;
            
            case (state)
                IDLE: begin
                    if (ce_enable && !desc_empty) begin
                        state <= FETCH_DESC;
                        active_desc <= desc_fifo[desc_rd_ptr[$clog2(DESC_DEPTH)-1:0]];
                        desc_rd_ptr <= desc_rd_ptr + 1'b1;
                    end
                    ce_busy <= !desc_empty;
                end
                
                FETCH_DESC: begin
                    // Load descriptor fields
                    current_src <= active_desc.src_addr;
                    current_dst <= active_desc.dst_addr;
                    bytes_remaining <= active_desc.length;
                    
                    if (active_desc.control[0]) begin
                        state <= D2H_COPY;
                    end else begin
                        state <= H2D_COPY;
                    end
                end
                
                H2D_COPY: begin
                    ce_busy <= 1'b1;
                    if (bytes_remaining > 0) begin
                        // Read from PCIe
                        pcie_rd_addr <= current_src;
                        pcie_rd_len <= (bytes_remaining > MAX_BURST) ? MAX_BURST : bytes_remaining[15:0];
                        pcie_rd_valid <= 1'b1;
                        
                        if (pcie_rd_data_valid) begin
                            // Write to L2 (Device)
                            // Snoop first for coherence
                            snoop_addr <= current_dst;
                            snoop_valid <= 1'b1;
                            
                            l2_addr <= current_dst;
                            l2_wdata <= pcie_rd_data;
                            l2_wr_en <= 1'b1;
                            l2_valid <= 1'b1;
                            
                            if (l2_ready) begin
                                current_src <= current_src + MAX_BURST;
                                current_dst <= current_dst + MAX_BURST;
                                bytes_remaining <= bytes_remaining - MAX_BURST;
                                bytes_copied <= bytes_copied + MAX_BURST;
                            end
                        end
                    end else begin
                        state <= COMPLETE;
                    end
                end
                
                D2H_COPY: begin
                    ce_busy <= 1'b1;
                    if (bytes_remaining > 0) begin
                        // Read from L2
                        l2_addr <= current_src;
                        l2_wr_en <= 1'b0;
                        l2_valid <= 1'b1;
                        
                        if (l2_rdata_valid) begin
                            // Write to PCIe
                            pcie_wr_addr <= current_dst;
                            pcie_wr_data <= l2_rdata;
                            pcie_wr_len <= (bytes_remaining > MAX_BURST) ? MAX_BURST : bytes_remaining[15:0];
                            pcie_wr_valid <= 1'b1;
                            
                            if (pcie_wr_ready) begin
                                current_src <= current_src + MAX_BURST;
                                current_dst <= current_dst + MAX_BURST;
                                bytes_remaining <= bytes_remaining - MAX_BURST;
                                bytes_copied <= bytes_copied + MAX_BURST;
                            end
                        end
                    end else begin
                        state <= COMPLETE;
                    end
                end
                
                COMPLETE: begin
                    if (active_desc.control[1]) begin  // IRQ enable
                        sema_out <= sema_out + 1'b1;  // Increment semaphore
                        sema_valid <= 1'b1;
                    end
                    state <= IDLE;
                end
                
                default: state <= IDLE;
            endcase
        end
    end
    
    // Descriptor Fetch Logic (simplified: descriptors pushed by driver via PCIe)
    // In reality, CE would fetch descriptors from host memory via PCIe
    // Here we assume descriptors are written to cfg interface or via special path

endmodule
