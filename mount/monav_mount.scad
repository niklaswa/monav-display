// Monav display mount: holds the Waveshare ESP32-S3-Touch-AMOLED-1.75 on the 17 mm ball head of
// the Model 3 Highland vent clip (the one that comes with the magnetic phone holder).
//
// Three printed parts, plus 3x M2x8 screws into the board's own standoffs:
//   ring - frame around the glass with the USB-C opening, BOOT/PWR pin holes and three ears
//          the board is screwed to
//   lid  - back cover with a slotted, threaded ball socket
//   nut  - knob that runs down the thread and squeezes the socket fingers onto the ball
//
// Board geometry comes from Waveshare's STEP model, see board_dummy.scad. Frame: X/Y as seen
// from behind the display, Z = depth behind the glass front (glass at Z = 0, mount towards +Z).

include <BOSL2/std.scad>
include <BOSL2/threading.scad>
use <board_dummy.scad>

/* [Output] */
part = "assembly"; // [assembly, section, ring, lid, nut, test_socket, print_plate]
// Cut plane for part = "section": "x" cuts through the USB-C port, "y" through the top ear
section_cut = "x"; // [x, y]

/* [Ball head] */
ball_d = 17;
// Radial gap between ball and socket before the nut is tightened
ball_gap = 0.1;
// How far the socket fingers reach past the ball's equator (snap-in and retention)
ball_wrap = 2.8;

/* [Socket thread] */
// Sets the finger wall: 24 left 2.1 mm under the thread root, too stiff to close by hand
thread_d = 23;
thread_pitch = 2;
// Printer clearance; BOSL2 widens the nut thread by 4 * $slop
$slop = 0.12;

/* [Buttons] */
// "pin": holes to press BOOT/PWR with a paper clip, "none": closed wall
buttons = "pin"; // [pin, none]

/* [Quality] */
$fa = 2;
$fs = 0.3;

/* [Hidden] */
eps = 0.01;

// --- Board (Waveshare STEP model) -------------------------------------------------------------
glass_d = 48.96;              // cover glass, full diameter for its whole 2.05 mm
standoff_z = 10.40;           // top of the three M2 SMT standoffs
header_z = 12.70;             // top of the soldered 8-pin female header, the tallest part
standoffs = [[0, -20.5], [13.75, 14.7], [-13.75, 14.7]];
usb_pos = [0.02, 8.07];       // USB-C receptacle centre (Y, Z); its face is at X = -23.8
button_angles = [33.6, -33.6];  // BOOT, PWR (side switches, plunger tip at r = 22.8)
button_z = 7.8;

// --- Case ---------------------------------------------------------------------------------------
glass_gap = 0.25;             // radial
rim_proud = 0.2;              // rim stands this far in front of the glass
rim_wall = 1.6;
glass_seat_z = 3.0;           // end of the glass recess (glass is 2.05, panel tabs reach 2.7)
bore_d = 47.0;                // PCB is 46.02, the SD socket reaches r = 23.27
back_z = header_z + 0.5;      // ring back face = lid inner face
lid_t = 2.4;
ear_w = [4.4, 5.4, 5.4];      // the top ear sits between the two JST connectors
screw_d = 2.4;
seam_chamfer = 0.4;
usb_cut = [13, 7.2];          // fits the overmould of a regular USB-C plug
usb_cut_r = 1.5;
pin_d = 2.2;
fpc_relief = [12, 24.9, 6.0];  // width, radius, depth: the panel's flex cable folds round the edge here
ring_od = glass_d + 2 * glass_gap + 2 * rim_wall;

// --- Ball socket (u = height above the lid's back face) ------------------------------------------
socket_r = ball_d / 2 + ball_gap;
ball_u = socket_r - 1.0;                          // the socket dips 1 mm into the lid
tip_u = ball_u + ball_wrap;
tip_r_in = sqrt(socket_r ^ 2 - ball_wrap ^ 2);
tip_r_out = tip_r_in + 1.3;
cone_ang = 30;                                    // clamp cone, half angle from the axis
root_r = (thread_d - 1.22687 * thread_pitch) / 2; // external thread minor radius
cone_u = tip_u - (root_r - tip_r_out) / tan(cone_ang);
thread_end_u = cone_u - 1.5;                      // plain neck between thread and cone
slot_w = 1.5;
// The fingers bend from here; below it the ball sits in a spherical cup that doesn't flex
slot_root_u = 1.0;
// Six narrow fingers close around the ball; four wide ones touched it only at their slot edges
slots = 6;

// --- Nut ----------------------------------------------------------------------------------------
nut_lip_r = ball_d / 2 + 0.35;                    // must slide over the ball
nut_contact_u = tip_u + (tip_r_out - nut_lip_r) / tan(cone_ang);  // nut top at first contact
nut_travel = 2.0;                                 // clamping travel past first contact
nut_h = nut_contact_u - nut_travel - 0.4;
nut_cone_r = root_r + 0.05;
nut_cone_h = (nut_cone_r - nut_lip_r) / tan(cone_ang);
nut_r = 15.8;
nut_lobe = 1.2;
nut_lobes = 6;

echo(str("ring OD ", ring_od, ", depth ", back_z + rim_proud + lid_t, " mm without socket; socket tip ",
         tip_u, ", nut height ", nut_h, ", nut touches at ", nut_contact_u, "; finger wall ", root_r - socket_r,
         " mm, tip ", tip_r_out - tip_r_in, " mm, length ", tip_u - slot_root_u, " mm"));

// --- Parts --------------------------------------------------------------------------------------

module front_ring() {
    difference() {
        union() {
            difference() {
                down(rim_proud) cyl(d = ring_od, h = back_z + rim_proud, anchor = BOTTOM,
                                    chamfer1 = 0.6, chamfer2 = seam_chamfer);
                down(rim_proud + eps) cylinder(d = glass_d + 2 * glass_gap, h = glass_seat_z + rim_proud + eps);
                // lead-in for the glass
                down(rim_proud + eps) cylinder(d1 = glass_d + 2 * glass_gap + 0.8, d2 = glass_d + 2 * glass_gap,
                                               h = 0.4 + eps);
                down(eps) cylinder(d = bore_d, h = back_z + 2 * eps);
            }
            ears();
        }
        screw_holes();
        translate([-ring_od / 2, usb_pos[0], usb_pos[1]])
            cuboid([8, usb_cut[0], usb_cut[1]], rounding = usb_cut_r, edges = "X");
        // the receptacle stands 0.8 mm proud of the PCB: a channel lets it pass when the board goes in
        translate([-24.2, usb_pos[0] - 5.6, glass_seat_z - eps]) cube([2, 11.2, usb_pos[1] - glass_seat_z]);
        translate([-fpc_relief[0] / 2, -fpc_relief[1], glass_seat_z - eps]) cube([fpc_relief[0], 3, fpc_relief[2] - glass_seat_z]);
        if (buttons == "pin")
            for (a = button_angles) zrot(a) translate([bore_d / 2 - 1, 0, button_z]) {
                yrot(90) cylinder(d = pin_d, h = 10);
                // countersink so the pin finds the hole
                right(ring_od / 2 - bore_d / 2 + 1 - 0.8) yrot(90) cylinder(d1 = pin_d, d2 = pin_d + 1.8, h = 0.8 + eps);
            }
    }
}

module ears() {
    for (i = [0 : len(standoffs) - 1]) {
        p = standoffs[i];
        wall_p = unit(p) * (bore_d / 2 + 1);
        up(standoff_z) linear_extrude(back_z - standoff_z)
            intersection() {
                hull() {
                    translate(p) circle(d = ear_w[i]);
                    translate(wall_p) circle(d = ear_w[i]);
                }
                circle(d = bore_d + 2);
            }
    }
}

module screw_holes() {
    for (p = standoffs) translate([p.x, p.y, standoff_z - 1]) cylinder(d = screw_d, h = back_z + lid_t);
}

module lid() {
    difference() {
        union() {
            up(back_z) cyl(d = ring_od, h = lid_t, anchor = BOTTOM, chamfer1 = seam_chamfer, rounding2 = 1.2);
            up(back_z + lid_t) socket_body();
        }
        up(back_z + lid_t) socket_cavity();
        screw_holes();
    }
}

module socket_body() {
    threaded_rod(d = thread_d, l = thread_end_u, pitch = thread_pitch, end_len1 = 0.4, anchor = BOTTOM);
    up(thread_end_u - eps) cylinder(r = root_r, h = cone_u - thread_end_u + eps);
    up(cone_u) cylinder(r1 = root_r, r2 = tip_r_out, h = tip_u - cone_u);
}

module socket_cavity() {
    up(ball_u) sphere(r = socket_r);
    // Hollows the fingers below the equator to an even wall. With the sphere alone they were 4 mm
    // thick at the root, which made them too stiff to close.
    up(slot_root_u) cylinder(r = socket_r, h = ball_u - slot_root_u + eps);
    // lead-in at the finger tips for the ball
    up(tip_u - 0.5) cylinder(r1 = tip_r_in, r2 = tip_r_in + 0.6, h = 0.5 + eps);
    for (i = [0 : slots - 1]) zrot(180 / slots + i * 360 / slots) {
        translate([0, -slot_w / 2, slot_root_u]) cube([thread_d, slot_w, tip_u]);
        up(slot_root_u) yrot(90) cylinder(d = slot_w, h = thread_d);
    }
}

function nut_profile() =
    [for (a = [0 : 1 : 359]) (nut_r + nut_lobe * cos(nut_lobes * a)) * [cos(a), sin(a)]];

// Nut in its own frame: bottom face (thread entry) at w = 0, clamp lip on top.
module nut() {
    difference() {
        offset_sweep(nut_profile(), height = nut_h, bottom = os_chamfer(width = 0.8), top = os_chamfer(width = 0.8));
        down(eps) threaded_rod(d = thread_d, l = nut_h - nut_cone_h + 2 * eps, pitch = thread_pitch,
                               internal = true, bevel1 = true, anchor = BOTTOM);
        up(nut_h - nut_cone_h) cylinder(r1 = nut_cone_r, r2 = nut_lip_r, h = nut_cone_h + eps);
        // opens the lip towards the ball's neck for more tilt
        up(nut_h - 0.6) cylinder(r1 = nut_lip_r, r2 = nut_lip_r + 0.6 + eps, h = 0.6 + eps);
    }
}

module test_socket() {
    difference() {
        union() {
            cyl(d = thread_d + 8, h = lid_t, anchor = BOTTOM, rounding2 = 1);
            up(lid_t) socket_body();
        }
        up(lid_t) socket_cavity();
    }
}

// --- Views and print layouts --------------------------------------------------------------------

module ball_dummy(tilt = 0) {
    up(back_z + lid_t + ball_u) yrot(tilt) {
        sphere(d = ball_d);
        cylinder(d = 9, h = ball_d / 2 + 8);
        up(ball_d / 2 + 8) cylinder(d1 = 9, d2 = 14, h = 4);
    }
}

// cut = "x" keeps Y >= 0, "y" keeps X >= 0, "" shows everything
module assembly(tilt = 0, tighten = 0.8, cut = "") {
    module keep() {
        if (cut == "x") translate([-50, 0, -10]) cube([100, 50, 60]);
        else if (cut == "y") translate([0, -50, -10]) cube([50, 100, 60]);
    }
    module piece(c, alpha = 1) {
        if (cut == "") color(c, alpha) children();
        else color(c, alpha) intersection() { children(); keep(); }
    }
    if (cut == "") board_dummy();
    else intersection() { board_dummy(); color("#555") keep(); }
    piece("#4b4f57") front_ring();
    piece("#5f646d") lid();
    piece("#2b2e33") up(back_z + lid_t + nut_contact_u - tighten - nut_h) nut();
    piece("white", 0.45) ball_dummy(tilt);
}

module print_ring() { up(back_z) xrot(180) front_ring(); }
module print_lid() { down(back_z) lid(); }

if (part == "assembly") {
    assembly(tilt = 20);
} else if (part == "section") {
    assembly(cut = section_cut);
} else if (part == "ring") {
    print_ring();
} else if (part == "lid") {
    print_lid();
} else if (part == "nut") {
    nut();
} else if (part == "test_socket") {
    test_socket();
} else if (part == "print_plate") {
    left(29) print_ring();
    right(29) print_lid();
    back(48) nut();
}
