import sys
# Writes a one-page PDF with two unnamed FreeText annotations whose appearance
# streams are ordinary (non-cached) file objects.
ap = b"BT /F1 10 Tf 2 5 Td (Hello) Tj ET"
objs = [
 b"<< /Type /Catalog /Pages 2 0 R >>",
 b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
 b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << >> /Annots [4 0 R 6 0 R] >>",
 b"<< /Type /Annot /Subtype /FreeText /Rect [100 700 200 720] /Contents (Hello) /DA (/Helv 10 Tf 0 g) /F 4 /AP << /N 5 0 R >> >>",
 b"<< /Type /XObject /Subtype /Form /BBox [0 0 100 20] /Resources << /Font << /F1 7 0 R >> >> /Length %d >>\nstream\n" % len(ap) + ap + b"\nendstream",
 b"<< /Type /Annot /Subtype /FreeText /Rect [100 600 200 620] /Contents (World) /DA (/Helv 10 Tf 0 g) /F 4 /AP << /N 8 0 R >> >>",
 b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
 b"<< /Type /XObject /Subtype /Form /BBox [0 0 100 20] /Resources << /Font << /F1 7 0 R >> >> /Length %d >>\nstream\n" % len(ap) + ap + b"\nendstream",
]
out = bytearray(b"%PDF-1.7\n"); offs = []
for i, o in enumerate(objs, 1):
    offs.append(len(out)); out += b"%d 0 obj\n" % i + o + b"\nendobj\n"
x = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs)+1) + b"".join(b"%010d 00000 n \n" % o for o in offs)
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs)+1, x)
open(sys.argv[1], "wb").write(out)
