# HDLC: where it came from, and what "point-to-point" means

HDLC (High-Level Data Link Control) grew out of IBM's SDLC work in the early 1970s. ISO
developed it into a broader family of link-control procedures. Its descendants include X.25's
LAPB and the framing used by PPP.

The important distinction is that **HDLC framing, a complete HDLC link protocol, and PPP are
different things**. They share mechanisms, but do not necessarily behave the same way.

<!-- contents: written by src/tools/gen_toc.py, do not edit by hand -->
**Contents**

- [The origins: IBM and SDLC](#the-origins-ibm-and-sdlc)
- [The standards timeline](#the-standards-timeline)
- [What HDLC actually specifies](#what-hdlc-actually-specifies)
- [Bit stuffing: the mechanism behind the flag](#bit-stuffing-the-mechanism-behind-the-flag)
- [Full HDLC also controls delivery](#full-hdlc-also-controls-delivery)
- [What "point-to-point HDLC" means](#what-point-to-point-hdlc-means)
- [PPP: where point-to-point and HDLC meet](#ppp-where-point-to-point-and-hdlc-meet)
- [HDLC-like framing over a UART](#hdlc-like-framing-over-a-uart)
- [Cisco HDLC is another separate case](#cisco-hdlc-is-another-separate-case)
- [Where this gateway fits](#where-this-gateway-fits)
- [Further reading](#further-reading)

<!-- /contents -->

## The origins: IBM and SDLC

IBM developed **Synchronous Data Link Control (SDLC)** for its communications systems. An early
technical account is R. A. Donnan and J. R. Kersey's *"Synchronous Data Link Control: A
Perspective"*, published in the **IBM Systems Journal, volume 13, number 2, 1974**. Describing
SDLC as simply "invented in 1975" therefore misses its earlier documented development. [1], [2]

The background was communication between computers, controllers and terminals over serial
lines. Earlier character-oriented protocols used special control characters to mark where a
message started and ended, and needed escaping rules when those characters turned up inside
the data.

The bit-oriented approach separated framing from character encoding: mark the ends of a frame
with a particular bit pattern, and stop that pattern from ever appearing by accident inside a
frame. The link no longer cared whether its contents were text, instructions or arbitrary
binary data. [3]

ISO's HDLC generalised this approach. IBM later described SDLC explicitly as conforming to an
**unbalanced normal operational subset of HDLC**. HDLC offered more choices of station
relationships, procedures and configurations than IBM's own implementation. [4]

## The standards timeline

HDLC came about through several standards and revisions, not one release.

| Date | Milestone | Significance |
|---|---|---|
| 1974 | IBM publishes its SDLC perspective paper | Contemporary documentation of the underlying work |
| January 1976 | First edition of ISO 3309 | ISO's catalogue records the first edition at this date |
| 1979 | ISO 3309-1979 and ISO 4335-1979 | Widely cited HDLC frame-structure and procedure specifications |
| 1979 | ANSI X3.66, ADCCP | Closely related American bit-oriented link-control standard |
| November 1989 | RFC 1134 | Early IETF PPP proposal |
| December 1993 | RFC 1549 | *PPP in HDLC Framing* |
| July 1994 | RFC 1661 and RFC 1662 | PPP, and its explicitly named *HDLC-like framing* |
| 2002 | ISO/IEC 13239:2002 | Consolidated HDLC specification |

So **1979 is not the earliest HDLC standard date**: ISO lists ISO 3309:1976 as edition 1. The
later consolidated specification covers frame structures, procedure elements and procedure
classes. [5], [6], [3], [7], [8], [9], [10]

## What HDLC actually specifies

HDLC is more than "put `0x7E` around a packet". Its specifications cover:

- frame boundaries and data transparency;
- addressing;
- control commands and responses;
- frame error detection;
- link set-up and operating modes;
- depending on the procedures chosen, sequencing, acknowledgements, flow control and
  retransmission.

Not every HDLC implementation has every option. IBM's documentation warns that two
implementations can both conform to HDLC and still choose subsets that cannot talk to each
other. [4], [10]

The familiar basic frame has these fields, in the order they are sent:

| Field | Purpose |
|---|---|
| Opening flag | Marks a frame boundary |
| Address | Identifies a station, or helps tell a command from a response |
| Control | Frame type and its control information |
| Information | The payload, when there is one |
| FCS | Frame Check Sequence, for error detection |
| Closing flag | Marks the frame boundary |

The flag is **`01111110`, hexadecimal `0x7E`**. ISO's frame-structure specification defines
where the fields go, the flags, transparency and the FCS. [4], [11]

## Bit stuffing: the mechanism behind the flag

Inside a bit-stuffed frame, the transmitter inserts a **zero after five one bits in a row**, and
the receiver removes it again.

So the contents of a frame can never contain the flag's six ones in a row. The insertion runs
across field and byte boundaries; the flags themselves are not stuffed. The FCS is calculated
before stuffing and checked after the receiver has removed it. [12]

This is called **bit stuffing**, or more exactly **zero-bit insertion**. It is a different
thing from sending idle flags between frames.

## Full HDLC also controls delivery

Its three main kinds of frame are:

| Kind | Meaning | Typical use |
|---|---|---|
| **I-frame** | Information | Numbered data, carrying acknowledgement information too |
| **S-frame** | Supervisory | Acknowledgement, receiver ready or not, retransmission requests |
| **U-frame** | Unnumbered | Link management and other unnumbered functions |

Examples of supervisory functions are **RR** (Receive Ready) and **RNR** (Receive Not Ready).
REJ and SREJ ask for retransmission, where they are supported.

Numbered operation lets several frames be on their way before the first is acknowledged. Basic
numbering allows up to seven outstanding frames, extended numbering up to 127.
Acknowledgements can ride along with data going the other way. [3]

## What "point-to-point HDLC" means

Point-to-point describes a connection with **two ends**. On its own it does not name one
particular HDLC protocol.

Two important arrangements are:

| Arrangement | How the stations relate |
|---|---|
| **Unbalanced operation** | A primary station controls the exchanges with a secondary station |
| **Balanced operation** | Both ends act as equals |

HDLC's **Asynchronous Balanced Mode (ABM)** gives the peer-to-peer point-to-point arrangement.
*Asynchronous* here is about which station may speak when; it does **not** mean UART start/stop
transmission. A balanced HDLC link can run on a synchronous, clocked line. [4], [10], [13]

**LAPB (Link Access Procedure, Balanced)** is the X.25 member of this family. It uses
HDLC-style procedures for the link between the two ends. LAPD, used for ISDN signalling, is
another relative. These are particular uses of the wider family, not other names for PPP. [3]

## PPP: where point-to-point and HDLC meet

**PPP means Point-to-Point Protocol.** Its early IETF specification, RFC 1134 from November
1989, addressed the lack of a standard way for different vendors' equipment to carry several
network protocols over serial point-to-point links.

PPP has three main parts:

1. **Encapsulation**, to identify and carry network-layer packets.
2. **LCP (Link Control Protocol)**, to set up, configure and test the link.
3. **NCPs (Network Control Protocols)**, to configure the network protocols carried over it.
   [14]

Its operation covers link set-up, optional authentication, network-layer configuration and
finally shutting the link down. These PPP procedures are defined separately from the framing.
[9]

On an ordinary serial line, an uncompressed PPP frame looks like this:

| Field | Value or meaning |
|---|---|
| Flag | `7E` |
| Address | `FF`, the all-stations address |
| Control | `03`, Unnumbered Information |
| Protocol | What the PPP packet carries |
| Information | The packet |
| FCS | Error-detection checksum |
| Flag | `7E` |

PPP does not need addressed stations on a link with only two ends; it keeps the
HDLC-compatible address and control fields in this basic format anyway. [8]

**Ordinary PPP does not use HDLC's numbered I-frames, acknowledgements and retransmission.**
Its HDLC-like framing detects a damaged frame; it does not repair lost data. That is why the
difference between *HDLC* and *HDLC-like framing* matters. [12]

## HDLC-like framing over a UART

PPP supports both bit-stuffed and octet-stuffed framing. On a UART-style asynchronous line,
**octet stuffing** (also called **byte stuffing**) sends the escape byte `0x7D` followed by the
original byte XOR `0x20`:

| Original byte | Bytes sent |
|---|---|
| `7E` | `7D 5E` |
| `7D` | `7D 5D` |

Other control characters can be escaped as well, as listed in the **Asynchronous Control
Character Map (ACCM)**. This helps with equipment that acts on characters such as XON/XOFF.
[12]

**Using `7E`, `7D`, XOR `20` and an FCS does not make a protocol PPP.** PPP also needs its
packet formats and control procedures. A protocol of your own can reuse the framing alone.

## Cisco HDLC is another separate case

Cisco HDLC is a proprietary synchronous serial encapsulation. Cisco documents its use of
**SLARP (Serial Line Address Resolution Protocol) for keepalives**, and its extension also
names the network protocol inside each frame.

So when a router offers **HDLC or PPP**, it usually means **Cisco HDLC or PPP**, not "the whole
ISO HDLC standard against PPP". [15], [16]

## Where this gateway fits

The HDLCGW board uses both kinds of framing described above:

- **On the line** to the Norsk Data machine it sends real bit-stuffed HDLC: `0x7E` flags, zero
  insertion after five ones, a 16-bit FCS, on a clocked synchronous line.
- **On USB and TCP** to the PC it uses the RFC 1662 byte stuffing (`7E` flags, `7D` escape, XOR
  `20`) with the same FCS. That is HDLC-like framing only, as in the section above: it is not
  PPP, and the gateway itself does not number, acknowledge or resend frames.
- **The link procedure** between ND machines (the XMSG traffic) is LAPB, the balanced HDLC
  procedure from X.25. The HDLCGW firmware passes those frames through unchanged; the XMSGGW
  firmware runs LAPB itself.

## Further reading

The most useful original documents:

- [IBM SDLC Concepts, June 1986](https://bitsavers.trailing-edge.com/pdf/ibm/datacomm/GA27-3093-3_SDLC_Concepts_Jun86.pdf):
  the architecture, frame handling and how SDLC relates to HDLC.
- [RFC 935: Reliable Link Layer Protocols](https://www.rfc-editor.org/rfc/rfc935.html): a 1985
  explanation of the competing approaches, written at the time.
- [RFC 1134: the original PPP proposal](https://www.rfc-editor.org/rfc/rfc1134.html): why PPP
  was made and its early design.
- [RFC 1661: PPP](https://www.rfc-editor.org/rfc/rfc1661.html): how the link runs and is
  negotiated.
- [RFC 1662: PPP in HDLC-like Framing](https://www.rfc-editor.org/rfc/rfc1662.html): bit
  stuffing, byte escaping and how to compute the FCS.
- [ISO/IEC 13239:2002](https://www.iso.org/standard/37010.html): the consolidated HDLC standard.
  The catalogue description is free to read; the standard itself is paid.

[1]: https://www.bitsavers.org/pdf/ibm/IBM_Systems_Journal/132/ibmsj1302D.pdf
[2]: https://www.bitsavers.org/pdf/ibm/IBM_Systems_Journal/134/ibmsj1304I.pdf
[3]: https://www.rfc-editor.org/rfc/rfc935.html
[4]: https://bitsavers.trailing-edge.com/pdf/ibm/datacomm/GA27-3093-3_SDLC_Concepts_Jun86.pdf
[5]: https://www.iso.org/standard/2474.html
[6]: https://www.rfc-editor.org/info/rfc1662/
[7]: https://www.rfc-editor.org/rfc/rfc1134.html
[8]: https://www.rfc-editor.org/rfc/rfc1549.html
[9]: https://www.rfc-editor.org/rfc/rfc1661.html
[10]: https://www.iso.org/standard/37010.html
[11]: https://www.iso.org/standard/8561.html
[12]: https://www.rfc-editor.org/rfc/rfc1662.html
[13]: https://www.juniper.net/documentation/us/en/software/junos/interfaces-security-devices/topics/topic-map/security-interface-interface-encap-overview.html
[14]: https://www.rfc-editor.org/info/rfc1134/
[15]: https://www.cisco.com/c/en/us/td/docs/routers/access/800M/software/800MSCG/serconf.html
[16]: https://cisco.tu-sofia.bg/wp-content/uploads/courses/CCNA4/course/module3/3.1.2.2/3.1.2.2.html
