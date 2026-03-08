// ============================================================================
// Module: warp_scheduler
// Description: Warp-level scheduler, selects 1 ready warp from 32
// Priority: Lower warp ID has higher priority (or configurable)
// ============================================================================
module warp_scheduler #(
    parameter NUM_WARPS = 32,
    parameter WARP_ID_W = $clog2(NUM_WARPS)
) (
    input  wire                  clk,
    input  wire                  rst_n,
    
    // Warp Status
    input  wire [NUM_WARPS-1:0]  warp_valid,    // Active warps (have threads to run)
    input  wire [NUM_WARPS-1:0]  warp_ready,    // Ready warps (no dependencies, not stalled)
    
    // Scheduling Control
    input  wire                  stall,         // Backend stall, hold current selection
    output reg  [WARP_ID_W-1:0]  warp_id,       // Selected warp ID
    output reg                   schedule_valid // Valid selection this cycle
);

    // Internal state
    //reg [NUM_WARPS-1:0] priority_mask;  // For round-robin (optional extension)
    wire [NUM_WARPS-1:0] eligible_warps;
    wire [WARP_ID_W-1:0] selected_id;
    wire                 has_selection;
    
    // Eligible: Must be both valid and ready
    assign eligible_warps = warp_valid & warp_ready;
    
    // Priority Encoder: Select lowest ID first (can be changed to RR)
    // Using priority encoder from Level 6
    priority_encoder #(
        .N(NUM_WARPS),
        .W(WARP_ID_W)
    ) u_prio_enc (
        .in(eligible_warps),
        .out(selected_id),
        .valid(has_selection)
    );
    
    // Output Logic with Stall Support
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            warp_id        <= {WARP_ID_W{1'b0}};
            schedule_valid <= 1'b0;
        end else begin
            if (!stall) begin
                // Update selection every cycle when not stalled
                warp_id        <= selected_id;
                schedule_valid <= has_selection;
            end
            // When stalled, hold current values (do nothing)
        end
    end

endmodule
