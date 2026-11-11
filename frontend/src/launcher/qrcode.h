/* Codice QR per un testo breve (un indirizzo web): modo byte, correzione M, versioni 1-10
   (fino a 213 byte). Scritto seguendo la norma ISO/IEC 18004, con la scelta della maschera
   per penalità. Restituisce il lato in moduli (21..57), 0 se il testo è troppo lungo;
   out riceve lato*lato byte, 1 = scuro. */
#pragma once

enum { QR_MAX_SIDE = 57 };

int qr_encode(const char *text, unsigned char out[QR_MAX_SIDE * QR_MAX_SIDE]);
