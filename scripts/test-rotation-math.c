/* Host-side verification of the Kindle renderer rotation math.
 * Mirrors the mapping in video/kindle.cpp: rebuild tables, convert a
 * synthetic "damage rect" and check that every view pixel addresses the
 * correct source pixel for all four rotations, and that the damage -> view
 * bounding box covers exactly the rotated damage. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static int streamWidth = 8, streamHeight = 5; /* tiny stream, W != H */
static int viewRotate = 0;
static int viewWidth, viewHeight;
static int *columnMap, *rowMap, *colIdx, *rowBaseArr, *colBaseArr, *rowIdx;
static unsigned char workLuma[8 * 5];
static unsigned char viewGray[8 * 5]; /* max size */

static void effectiveDims(int *ew, int *eh) {
  if (viewRotate % 180) { *ew = streamHeight; *eh = streamWidth; }
  else { *ew = streamWidth; *eh = streamHeight; }
}

/* rebuildRenderState's table construction (letterbox scale = 1 here). */
static void rebuild(int rot) {
  viewRotate = rot;
  int effW, effH;
  effectiveDims(&effW, &effH);
  viewWidth = effW;
  viewHeight = effH;
  columnMap = malloc(sizeof(int) * viewWidth);
  rowMap = malloc(sizeof(int) * viewHeight);
  colIdx = malloc(sizeof(int) * viewWidth);
  colBaseArr = malloc(sizeof(int) * viewWidth);
  rowBaseArr = malloc(sizeof(int) * viewHeight);
  rowIdx = malloc(sizeof(int) * viewHeight);
  for (int vx = 0; vx < viewWidth; vx++)
    columnMap[vx] = effW > 0 ? (vx * effW) / viewWidth : 0; /* simple 1:1 */
  for (int vy = 0; vy < viewHeight; vy++)
    rowMap[vy] = effH > 0 ? (vy * effH) / viewHeight : 0;
  for (int vx = 0; vx < viewWidth; vx++) {
    int ex = columnMap[vx];
    switch (viewRotate) {
      case 90:  colBaseArr[vx] = (streamHeight - 1 - ex) * streamWidth; colIdx[vx] = 0; break;
      case 180: colBaseArr[vx] = 0; colIdx[vx] = streamWidth - 1 - ex; break;
      case 270: colBaseArr[vx] = ex * streamWidth; colIdx[vx] = 0; break;
      default:  colBaseArr[vx] = 0; colIdx[vx] = ex; break;
    }
  }
  for (int vy = 0; vy < viewHeight; vy++) {
    int ey = rowMap[vy];
    switch (viewRotate) {
      case 90:  rowBaseArr[vy] = 0; rowIdx[vy] = ey; break;
      case 180: rowBaseArr[vy] = (streamHeight - 1 - ey) * streamWidth; rowIdx[vy] = 0; break;
      case 270: rowBaseArr[vy] = 0; rowIdx[vy] = streamWidth - 1 - ey; break;
      default:  rowBaseArr[vy] = ey * streamWidth; rowIdx[vy] = 0; break;
    }
  }
}

static void convertViewFull(int *pvx0, int *pvy0, int *pvx1, int *pvy1) {
  *pvx0 = 0; *pvy0 = 0; *pvx1 = viewWidth; *pvy1 = viewHeight;
  const bool direct = (viewRotate % 180) == 0;
  for (int vy = 0; vy < viewHeight; vy++) {
    for (int vx = 0; vx < viewWidth; vx++) {
      unsigned char v = direct ? workLuma[rowBaseArr[vy] + colIdx[vx]]
                               : workLuma[colBaseArr[vx] + rowIdx[vy]];
      viewGray[vy * viewWidth + vx] = v;
    }
  }
}

/* Forward reference model: view(ex,ey) should show stream pixel R(ex,ey). */
static unsigned char expectedPixel(int ex, int ey) {
  int sx, sy;
  switch (viewRotate) {
    case 90:  sx = ey; sy = streamHeight - 1 - ex; break;
    case 180: sx = streamWidth - 1 - ex; sy = streamHeight - 1 - ey; break;
    case 270: sx = streamWidth - 1 - ey; sy = ex; break;
    default:  sx = ex; sy = ey; break;
  }
  return workLuma[sy * streamWidth + sx];
}

/* pushToPanel's stream-damage -> effective-rect mapping. */
static void effRectFromStream(int sx0, int sy0, int sx1, int sy1,
                              int *ex0, int *ey0, int *ex1, int *ey1) {
  switch (viewRotate) {
    case 90:  *ex0 = streamHeight - sy1; *ex1 = streamHeight - sy0;
              *ey0 = sx0; *ey1 = sx1; break;
    case 180: *ex0 = streamWidth - sx1; *ex1 = streamWidth - sx0;
              *ey0 = streamHeight - sy1; *ey1 = streamHeight - sy0; break;
    case 270: *ex0 = sy0; *ex1 = sy1;
              *ey0 = streamWidth - sx1; *ey1 = streamWidth - sx0; break;
    default:  *ex0 = sx0; *ex1 = sx1; *ey0 = sy0; *ey1 = sy1; break;
  }
}

int main(void) {
  for (int i = 0; i < 8 * 5; i++)
    workLuma[i] = (unsigned char)(i * 7 + 3);

  for (int rot = 0; rot < 360; rot += 90) {
    rebuild(rot);
    int vx0, vy0, vx1, vy1;
    convertViewFull(&vx0, &vy0, &vx1, &vy1);

    int failures = 0;
    for (int vy = 0; vy < viewHeight; vy++)
      for (int vx = 0; vx < viewWidth; vx++) {
        unsigned char got = viewGray[vy * viewWidth + vx];
        unsigned char want = expectedPixel(vx, vy);
        if (got != want) {
          if (failures < 3)
            fprintf(stderr, "rot %d: view(%d,%d) got %d want %d\n",
                    rot, vx, vy, got, want);
          failures++;
        }
      }
    printf("rot %3d: view %dx%d, pixel check %s (%d mismatches)\n",
           rot, viewWidth, viewHeight, failures ? "FAIL" : "OK", failures);

    /* Damage rect mapping: damage covers stream rows 1..3, cols 2..6.
     * The eff rect must be exactly the rotated bounding box. */
    int ex0, ey0, ex1, ey1;
    effRectFromStream(2, 1, 7, 4, &ex0, &ey0, &ex1, &ey1);
    int ew, eh;
    effectiveDims(&ew, &eh);
    /* Independent forward model: eff rect = bounding box of R(stream rect). */
    int fx0 = ew, fy0 = eh, fx1 = -1, fy1 = -1;
    for (int sy = 1; sy < 4; sy++)
      for (int sx = 2; sx < 7; sx++) {
        int px, py;
        switch (viewRotate) {
          case 90:  px = streamHeight - 1 - sy; py = sx; break;
          case 180: px = streamWidth - 1 - sx; py = streamHeight - 1 - sy; break;
          case 270: px = sy; py = streamWidth - 1 - sx; break;
          default:  px = sx; py = sy; break;
        }
        if (px < fx0) fx0 = px;
        if (px > fx1) fx1 = px;
        if (py < fy0) fy0 = py;
        if (py > fy1) fy1 = py;
      }
    fx1++; fy1++; /* inclusive -> exclusive */
    printf("          damage(2..7,1..4) -> eff [%d,%d)x[%d,%d), want [%d,%d)x[%d,%d) %s\n",
           ex0, ex1, ey0, ey1, fx0, fx1, fy0, fy1,
           (ex0 == fx0 && ex1 == fx1 && ey0 == fy0 && ey1 == fy1) ? "OK" : "FAIL");

    /* Touch mapping round trip: stream -> view expectation via inverse. */
    {
      bool ok = true;
      for (int sy = 0; sy < streamHeight && ok; sy++)
        for (int sx = 0; sx < streamWidth && ok; sx++) {
          /* view point that shows stream (sx,sy): invert sendMousePosition. */
          int ex, ey;
          switch (viewRotate) {
            case 90:  ex = streamHeight - 1 - sy; ey = sx; break;
            case 180: ex = streamWidth - 1 - sx; ey = streamHeight - 1 - sy; break;
            case 270: ex = sy; ey = streamWidth - 1 - sx; break;
            default:  ex = sx; ey = sy; break;
          }
          int px, py;
          switch (viewRotate) {
            case 90:  px = ey; py = streamHeight - 1 - ex; break;
            case 180: px = streamWidth - 1 - ex; py = streamHeight - 1 - ey; break;
            case 270: px = streamWidth - 1 - ey; py = ex; break;
            default:  px = ex; py = ey; break;
          }
          if (px != sx || py != sy) {
            fprintf(stderr, "rot %d touch round trip fails at stream(%d,%d)\n", rot, sx, sy);
            ok = false;
          }
        }
      printf("          touch round trip %s\n", ok ? "OK" : "FAIL");
    }
    free(columnMap); free(rowMap); free(colIdx);
    free(colBaseArr); free(rowBaseArr); free(rowIdx);
  }
  return 0;
}
