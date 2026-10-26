`timescale 1ns/1ps
/*
  Does the substrate stay in_pic the picture?

  Black in everywhere - the colour ascal fills a pillarbox with - and count the
  pixels that come out lit, sampling only well away from the region boundaries so
  the five-stage pipeline cannot confuse which region a pixel belonged to.

  Two failure modes it is here to catch: an inverted gate, which would put the
  lattice ONLY in the border, and a gate that kills the feature, which would leave
  the picture count at zero. Both are reported rather than passing quietly.
*/
module tb;
	reg clk = 0, clk_sys = 0;
	always #2 clk = ~clk;
	always #3 clk_sys = ~clk_sys;

	reg         cmd_wr = 0;
	reg  [15:0] cmd_in = 0;
	reg  [23:0] din = 24'h000000;
	reg         hs_in = 1, vs_in = 1, de_in = 0, brd_in = 1;
	wire [23:0] dout;

	shadowmask dut (.clk(clk), .clk_sys(clk_sys), .cmd_wr(cmd_wr), .cmd_in(cmd_in),
		.din(din), .hs_in(hs_in), .vs_in(vs_in), .de_in(de_in), .brd_in(brd_in),
		.enable(1'b1), .dout(dout), .hs_out(), .vs_out(), .de_out());

	task wr(input [2:0] op, input [11:0] val);   // opcode [15:13], payload [11:0]
	begin
		@(posedge clk_sys); cmd_wr <= 1; cmd_in <= {op, 1'b0, val};
		@(posedge clk_sys); cmd_wr <= 0;
	end
	endtask

	integer ln, k, region, in_pic;
	integer lit_b = 0, lit_p = 0, seen_b = 0, seen_p = 0;
	localparam N = 100, MARGIN = 12;

	initial begin
		wr(3'b000, 12'h008);   // enable is cmd_in[3]; also resets the LUT index
		wr(3'b001, 12'd1);     // vmax 1 -> 2 rows
		wr(3'b010, 12'd1);     // hmax 1 -> 2 cols
		wr(3'b100, 12'hE14);
		wr(3'b101, 12'h8B9);   // substrate 0x8B9E14
		wr(3'b110, 12'd3);     // blend 3/4, the shipping value
		/*
		  mask_idx is {vindex, hindex}, so the LUT is strided by 16, not packed:
		  a 2x2 mask lives at 0,1 and 16,17. Writing 0..3 leaves the picture reading
		  uninitialised entries, which shows up as X on dout rather than as a wrong
		  colour - the first version of this test did exactly that and its "every
		  pixel lit" was counting X, not substrate.
		*/
		wr(3'b011, 12'hF20);   // idx 0  : row 0, col 0 - substrate
		wr(3'b011, 12'h700);   // idx 1  : row 0, col 1 - unity
		for (k = 2; k < 16; k = k + 1) wr(3'b011, 12'h700);
		wr(3'b011, 12'hF20);   // idx 16 : row 1, col 0 - substrate
		wr(3'b011, 12'h700);   // idx 17 : row 1, col 1 - unity
		repeat (20) @(posedge clk);

		de_in <= 1;
		/*
		  vcount is only cleared on a vsync FALLING edge, so without a vsync it stays
		  uninitialised, mask_idx reads X, and every product downstream is X - which a
		  "not black" test happily counts as lit. Pulse it like a real frame does.
		*/
		vs_in <= 0; repeat (4) @(posedge clk); vs_in <= 1; repeat (4) @(posedge clk);
		/*
		  vcount is advanced by `next_v`, which is itself only written on a border
		  RISING edge - so before the first such edge it is X, and the first hsync
		  computes vcount <= 0 + X and poisons the row index for the whole run. One
		  full border -> picture -> border transition defines both it and `pde`.
		*/
		brd_in <= 0; repeat (4) @(posedge clk); brd_in <= 1; repeat (4) @(posedge clk);
		for (ln = 0; ln < 6; ln = ln + 1) begin
			hs_in <= 0; @(posedge clk); @(posedge clk); hs_in <= 1;
			for (region = 0; region < 3; region = region + 1) begin
				in_pic = (region == 1);
				brd_in <= (in_pic != 0) ? 1'b0 : 1'b1;
				for (k = 0; k < N; k = k + 1) begin
					@(posedge clk);
					#1;
					if (in_pic && ln == 5 && k >= MARGIN && k < MARGIN + 6)
						$display("  px %0d: dout=%06h idx=%02h sub_sel=%b r_mul=%b subst=%06h blend=%0d en=%b",
							k - MARGIN, dout, dut.mask_idx, dut.sub_sel, dut.r_mul,
							dut.subst, dut.sub_blend, dut.mask_enable);
					if (k >= MARGIN && k < N - MARGIN) begin
						if (in_pic) begin
							seen_p = seen_p + 1;
							if (dout !== 24'h000000) lit_p = lit_p + 1;
						end else begin
							seen_b = seen_b + 1;
							if (dout !== 24'h000000) lit_b = lit_b + 1;
						end
					end
				end
			end
		end
		repeat (10) @(posedge clk);

		$display("lit inside the picture: %0d of %0d sampled", lit_p, seen_p);
		$display("lit in the border:      %0d of %0d sampled", lit_b, seen_b);
		if (lit_b != 0)      $display("FAIL - the substrate leaks into the border");
		else if (lit_p == 0) $display("FAIL - the substrate never fired; this proves nothing");
		else                 $display("PASS - border stays black, substrate fires only in the picture");
		$finish;
	end
endmodule
