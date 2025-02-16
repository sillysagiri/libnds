// SPDX-License-Identifier: Zlib
// SPDX-FileNotice: Modified from the original version by the BlocksDS project.
//
// Copyright (C) 2010 Richard Eric M. Lope BSN RN (Relminator)

// Easy GL2D
//
// http://rel.betterwebber.com
//
// A very small and simple DS rendering lib using the 3d core to render 2D stuff

#include <gl2d.h>

// Our static global variable used for depth values since we cannot disable
// depth testing in the DS hardware. This value is incremented for every draw
// call.
static v16 g_depth = 0;
int gCurrentTexture = 0;

void glScreen2D(void)
{
    // Initialize gl
    glInit();

    // Enable textures
    glEnable(GL_TEXTURE_2D);

    // Enable antialiasing
    glEnable(GL_ANTIALIAS);

    // Setup the rear plane
    glClearColor(0, 0, 0, 31); // BG must be opaque for AA to work
    glClearPolyID(63); // BG must have a unique polygon ID for AA to work

    glClearDepth(GL_MAX_DEPTH);

    // This should work the same as the normal gl call
    glViewport(0, 0, 255, 191);

    // Any floating point gl call is being converted to fixed prior to being
    // implemented
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(70, 256.0 / 192.0, 1, 200);

    gluLookAt(0.0, 0.0, 1.0,  // Camera position
              0.0, 0.0, 0.0,  // Look at
              0.0, 1.0, 0.0); // Up

    glMaterialf(GL_AMBIENT, RGB15(31, 31, 31));
    glMaterialf(GL_DIFFUSE, RGB15(31, 31, 31));
    glMaterialf(GL_SPECULAR, BIT(15) | RGB15(31, 31, 31));
    glMaterialf(GL_EMISSION, RGB15(31, 31, 31));

    // The DS uses a table for shinyness, this generates one
    glMaterialShinyness();

    // Polygon attributes
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_BACK);
}

void glBegin2D(void)
{
    // Reset texture matrix just in case we did some funky stuff with it
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();

    // Set orthographic projection at 1:1 correspondence to screen coords.
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();

    // glOrtho expects f32 values but if we use the standard f32 values, we need
    // to rescale either every vert or the modelview matrix by the same amount
    // to make it work. That's gonna give us lots of overflows and headaches. So
    // we "scale down" and use an all integer value.
    //
    // The projection matrix actually thinks that the size of the DS is
    // (256 << factor) x (192 << factor). After this, we scale the MODELVIEW
    // matrix to match this scale factor.
    //
    // This way, it is possible to draw on the screen by using numbers up to 256
    // x 192, but internally the DS has more digits when it does transformations
    // like a rotation. Not having this factor results in noticeable flickering,
    // specially in some emulators.
    //
    // Unfortunately, applying this factor reduces the accuracy of the Y
    // coordinate a lot (nothing is noticeable in the X coordinate). Any factor
    // over 4 starts showing a noticeable accuracy loss: some sprites start
    // being slightly distorted, with missing some horizontal lines as the
    // height is reduced. When the number is higher, like 12, the Y coordinate
    // is significantly compressed. When the number is even higher, like 18, the
    // polygons disappear because too much accuracy has been lost.
    //
    // The current solution is to compromise, and use a factor of 2, which
    // doesn't cause any distortion, and solves most of the flickering. Ideally
    // we would use 0 to simplify the calculations, but we want to reduce the
    // flickering.
    //
    // On hardware, the difference in flickering between 0 and 2 isn't too
    // noticeable, but it is noticeable. In DeSmuMe it is very noticeable.
    // In my tests, Y axis distortion starts to happen with a factor of 4, so a
    // factor of 2 should be safe and reduce enough flickering.

    int factor = 2;

    // Downscale projection matrix
    glOrthof32(0, SCREEN_WIDTH << factor, SCREEN_HEIGHT << factor, 0, -inttof32(1), inttof32(1));

    // Reset modelview matrix. No need to scale up by << 12
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    MATRIX_SCALE = inttof32(1 << factor);
    MATRIX_SCALE = inttof32(1 << factor);
    MATRIX_SCALE = inttof32(1);

    // What?!! No glDisable(GL_DEPTH_TEST)?!!!!!!
    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_ANTIALIAS);    // Disable AA
    glDisable(GL_OUTLINE);      // Disable edge-marking

    glColor(0x7FFF);            // White

    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE); // No culling

    gCurrentTexture = 0; // Set current texture to 0
    // Set depth to 0. We need this var since we cannot disable depth testing
    g_depth = 0;
}

void glEnd2D(void)
{
    // Restore 3d matrices and set current matrix to modelview
    glMatrixMode(GL_PROJECTION);
    glPopMatrix(1);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix(1);
}

void glPutPixel(int x, int y, int color)
{
    glBindTexture(0, 0);
    glColor(color);
    glBegin(GL_TRIANGLES);
        glVertex3v16(x, y, g_depth);
        glVertex2v16(x, y);
        glVertex2v16(x, y);
    glEnd();
    glColor(0x7FFF);
    g_depth++;
    gCurrentTexture = 0;
}

void glLine(int x1, int y1, int x2, int y2, int color)
{
    x2++;
    y2++;

    glBindTexture(0, 0);
    glColor(color);
    glBegin(GL_TRIANGLES);
        glVertex3v16(x1, y1, g_depth);
        glVertex2v16(x2, y2);
        glVertex2v16(x2, y2);
    glEnd();
    glColor(0x7FFF);
    g_depth++;
    gCurrentTexture = 0;
}

void glBox(int x1, int y1, int x2, int y2, int color)
{
    x2++;
    y2++;

    glBindTexture(0, 0);
    glColor(color);
    glBegin(GL_TRIANGLES);

        glVertex3v16(x1, y1, g_depth);
        glVertex2v16(x2, y1);
        glVertex2v16(x2, y1);

        glVertex2v16(x2, y1);
        glVertex2v16(x2, y2);
        glVertex2v16(x2, y2);

        // Bug fix for lower-right corner disappearing pixel
        glVertex2v16(++x2, y2);
        glVertex2v16(x1, y2);
        glVertex2v16(x1, y2);

        glVertex2v16(x1, y2);
        glVertex2v16(x1, y1);
        glVertex2v16(x1, y1);

    glEnd();
    glColor(0x7FFF);
    g_depth++;
    gCurrentTexture = 0;

}

void glBoxFilled(int x1, int y1, int x2, int y2, int color)
{
    x2++;
    y2++;

    glBindTexture(0, 0);
    glColor(color);
    glBegin(GL_QUADS);
        // Use 3i for first vertex so that we increment HW depth
        glVertex3v16(x1, y1, g_depth);
        // No need for 3 vertices as 2i would share last depth call
        glVertex2v16(x1, y2);
        glVertex2v16(x2, y2);
        glVertex2v16(x2, y1);
    glEnd();
    glColor(0x7FFF);
    g_depth++;
    gCurrentTexture = 0;
}

void glBoxFilledGradient(int x1, int y1, int x2, int y2,
                         int color1, int color2, int color3, int color4)
{
    x2++;
    y2++;

    glBindTexture(0,0);
    glBegin(GL_QUADS);
        glColor(color1);
        // Use 3i for first vertex so that we increment HW depth
        glVertex3v16(x1, y1, g_depth);
        glColor(color2);
        // No need for 3 vertices as 2i would share last depth call
        glVertex2v16(x1, y2);
        glColor(color3);
        glVertex2v16(x2, y2);
        glColor(color4);
        glVertex2v16(x2, y1);
    glEnd();
    glColor(0x7FFF);
    g_depth++;
    gCurrentTexture = 0;
}

void glTriangle(int x1, int y1, int x2, int y2, int x3, int y3, int color)
{
    glBindTexture(0, 0);
    glColor(color);
    glBegin(GL_TRIANGLES);

        glVertex3v16(x1, y1, g_depth);
        glVertex2v16(x2, y2);
        glVertex2v16(x2, y2);

        glVertex2v16(x2, y2);
        glVertex2v16(x3, y3);
        glVertex2v16(x3, y3);

        glVertex2v16(x3, y3);
        glVertex2v16(x1, y1);
        glVertex2v16(x1, y1);

    glEnd();
    glColor(0x7FFF);
    g_depth++;
    gCurrentTexture = 0;
}

void glTriangleFilled(int x1, int y1, int x2, int y2, int x3, int y3, int color)
{
    glBindTexture(0, 0);
    glColor(color);
    glBegin(GL_TRIANGLES);
        // Use 3i for first vertex so that we increment HW depth
        glVertex3v16(x1, y1, g_depth);
        // No need for 3 vertices as 2i would share last depth call
        glVertex2v16(x2, y2);
        glVertex2v16(x3, y3);
    glEnd();
    glColor(0x7FFF);
    g_depth++;
    gCurrentTexture = 0;
}

void glTriangleFilledGradient(int x1, int y1, int x2, int y2, int x3, int y3,
                              int color1, int color2, int color3)
{
    glBindTexture(0, 0);
    glBegin(GL_TRIANGLES);
        // Use 3i for first vertex so that we increment HW depth
        glColor(color1);
        glVertex3v16(x1, y1, g_depth);
        glColor(color2);
        // No need for 3 vertices as 2i would share last depth call
        glVertex2v16(x2, y2);
        glColor(color3);
        glVertex2v16(x3, y3);
    glEnd();
    glColor(0x7FFF);
    g_depth++;
    gCurrentTexture = 0;
}

void glSprite(int x, int y, int flipmode, const glImage *spr)
{
    int x1 = x;
    int y1 = y;
    int x2 = x + spr->width;
    int y2 = y + spr->height;

    int u1 = spr->u_off + ((flipmode & GL_FLIP_H) ? spr->width - 1 : 0);
    int u2 = spr->u_off + ((flipmode & GL_FLIP_H) ? 0 : spr->width);
    int v1 = spr->v_off + ((flipmode & GL_FLIP_V) ? spr->height - 1 : 0);
    int v2 = spr->v_off + ((flipmode & GL_FLIP_V) ? 0 : spr->height);

    if (spr->textureID != gCurrentTexture)
    {
        glBindTexture(GL_TEXTURE_2D, spr->textureID);
        gCurrentTexture = spr->textureID;
    }

    glBegin(GL_QUADS);

        glTexCoord2i(u1, v1); glVertex3v16(x1, y1, g_depth);
        glTexCoord2i(u1, v2); glVertex2v16(x1, y2);
        glTexCoord2i(u2, v2); glVertex2v16(x2, y2);
        glTexCoord2i(u2, v1); glVertex2v16(x2, y1);

    glEnd();

    g_depth++;
}

void glSpriteScale(int x, int y, s32 scale, int flipmode, const glImage *spr)
{
    int x1 = 0;
    int y1 = 0;
    int x2 = spr->width;
    int y2 = spr->height;

    int u1 = spr->u_off + ((flipmode & GL_FLIP_H) ? spr->width - 1 : 0);
    int u2 = spr->u_off + ((flipmode & GL_FLIP_H) ? 0 : spr->width);
    int v1 = spr->v_off + ((flipmode & GL_FLIP_V) ? spr->height - 1 : 0);
    int v2 = spr->v_off + ((flipmode & GL_FLIP_V) ? 0 : spr->height);

    if (spr->textureID != gCurrentTexture)
    {
        glBindTexture(GL_TEXTURE_2D, spr->textureID);
        gCurrentTexture = spr->textureID;
    }

    glPushMatrix();

        glTranslatef32(x, y, 0);
        glScalef32(scale, scale, 1 << 12);

        glBegin(GL_QUADS);

            glTexCoord2i(u1, v1);
            glVertex3v16(x1, y1, g_depth);
            glTexCoord2i(u1, v2);
            glVertex2v16(x1, y2);
            glTexCoord2i(u2, v2);
            glVertex2v16(x2, y2);
            glTexCoord2i(u2, v1);
            glVertex2v16(x2, y1);

        glEnd();

    glPopMatrix(1);
    g_depth++;
}

void glSpriteScaleXY(int x, int y, s32 scaleX, s32 scaleY, int flipmode,
                     const glImage *spr)
{
    int x1 = 0;
    int y1 = 0;
    int x2 = spr->width;
    int y2 = spr->height;

    int u1 = spr->u_off + ((flipmode & GL_FLIP_H) ? spr->width - 1 : 0);
    int u2 = spr->u_off + ((flipmode & GL_FLIP_H) ? 0 : spr->width);
    int v1 = spr->v_off + ((flipmode & GL_FLIP_V) ? spr->height - 1 : 0);
    int v2 = spr->v_off + ((flipmode & GL_FLIP_V) ? 0 : spr->height);

    if (spr->textureID != gCurrentTexture)
    {
        glBindTexture(GL_TEXTURE_2D, spr->textureID);
        gCurrentTexture = spr->textureID;
    }

    glPushMatrix();

        glTranslatef32(x, y, 0);
        glScalef32(scaleX, scaleY, 1 << 12);

        glBegin(GL_QUADS);

            glTexCoord2i(u1, v1);
            glVertex3v16(x1, y1, g_depth);
            glTexCoord2i(u1, v2);
            glVertex2v16(x1, y2);
            glTexCoord2i(u2, v2);
            glVertex2v16(x2, y2);
            glTexCoord2i(u2, v1);
            glVertex2v16(x2, y1);

        glEnd();

    glPopMatrix(1);
    g_depth++;
}

void glSpriteRotate(int x, int y, s32 angle, int flipmode, const glImage *spr)
{
    int s_half_x = ((spr->width) + (spr->width & 1)) / 2;
    int s_half_y = ((spr->height) + (spr->height & 1)) / 2;

    int x1 = -s_half_x;
    int y1 = -s_half_y;

    int x2 = s_half_x;
    int y2 = s_half_y;

    int u1 = spr->u_off + ((flipmode & GL_FLIP_H) ? spr->width - 1 : 0);
    int u2 = spr->u_off + ((flipmode & GL_FLIP_H) ? 0 : spr->width);
    int v1 = spr->v_off + ((flipmode & GL_FLIP_V) ? spr->height - 1 : 0);
    int v2 = spr->v_off + ((flipmode & GL_FLIP_V) ? 0 : spr->height);

    if (spr->textureID != gCurrentTexture)
    {
        glBindTexture(GL_TEXTURE_2D, spr->textureID);
        gCurrentTexture = spr->textureID;
    }

    glPushMatrix();

        glTranslatef32(x, y, 0);
        glRotateZi(angle);

        glBegin(GL_QUADS);

            glTexCoord2i(u1, v1);
            glVertex3v16(x1, y1, g_depth);
            glTexCoord2i(u1, v2);
            glVertex2v16(x1, y2);
            glTexCoord2i(u2, v2);
            glVertex2v16(x2, y2);
            glTexCoord2i(u2, v1);
            glVertex2v16(x2, y1);

        glEnd();

    glPopMatrix(1);

    g_depth++;
}

void glSpriteRotateScale(int x, int y, s32 angle, s32 scale, int flipmode,
                         const glImage *spr)
{
    int s_half_x = ((spr->width) + (spr->width & 1)) / 2;
    int s_half_y = ((spr->height) + (spr->height & 1)) / 2;

    int x1 = -s_half_x;
    int y1 = -s_half_y;

    int x2 = s_half_x;
    int y2 = s_half_y;

    int u1 = spr->u_off + ((flipmode & GL_FLIP_H) ? spr->width - 1 : 0);
    int u2 = spr->u_off + ((flipmode & GL_FLIP_H) ? 0 : spr->width);
    int v1 = spr->v_off + ((flipmode & GL_FLIP_V) ? spr->height - 1 : 0);
    int v2 = spr->v_off + ((flipmode & GL_FLIP_V) ? 0 : spr->height);

    if (spr->textureID != gCurrentTexture)
    {
        glBindTexture(GL_TEXTURE_2D, spr->textureID);
        gCurrentTexture = spr->textureID;
    }

    glPushMatrix();

        glTranslatef32(x, y, 0);
        glScalef32(scale, scale, 1 << 12);
        glRotateZi(angle);


        glBegin(GL_QUADS);

            glTexCoord2i(u1, v1);
            glVertex3v16(x1, y1, g_depth);
            glTexCoord2i(u1, v2);
            glVertex2v16(x1, y2);
            glTexCoord2i(u2, v2);
            glVertex2v16(x2, y2);
            glTexCoord2i(u2, v1);
            glVertex2v16(x2, y1);

        glEnd();

    glPopMatrix(1);

    g_depth++;
}

void glSpriteRotateScaleXY(int x, int y, s32 angle, s32 scaleX, s32 scaleY,
                           int flipmode, const glImage *spr)
{

    int s_half_x = ((spr->width) + (spr->width & 1)) / 2;
    int s_half_y = ((spr->height) + (spr->height & 1))  / 2;

    int x1 = -s_half_x;
    int y1 = -s_half_y;

    int x2 = s_half_x;
    int y2 = s_half_y;

    int u1 = spr->u_off + ((flipmode & GL_FLIP_H) ? spr->width - 1 : 0);
    int u2 = spr->u_off + ((flipmode & GL_FLIP_H) ? 0 : spr->width);
    int v1 = spr->v_off + ((flipmode & GL_FLIP_V) ? spr->height - 1 : 0);
    int v2 = spr->v_off + ((flipmode & GL_FLIP_V) ? 0 : spr->height);

    if (spr->textureID != gCurrentTexture)
    {
        glBindTexture(GL_TEXTURE_2D, spr->textureID);
        gCurrentTexture = spr->textureID;
    }

    glPushMatrix();

        glTranslatef32(x, y, 0);
        glScalef32(scaleX, scaleY, 1 << 12);
        glRotateZi(angle);


        glBegin(GL_QUADS);

            glTexCoord2i(u1, v1);
            glVertex3v16(x1, y1, g_depth);
            glTexCoord2i(u1, v2);
            glVertex2v16(x1, y2);
            glTexCoord2i(u2, v2);
            glVertex2v16(x2, y2);
            glTexCoord2i(u2, v1);
            glVertex2v16(x2, y1);

        glEnd();

    glPopMatrix(1);

    g_depth++;
}

void glSpriteStretchHorizontal(int x, int y, int length_x, const glImage *spr)
{
    int x1 = x;
    int y1 = y;
    int x2 = x + length_x;
    int y2 = y + spr->height;
    int su = (spr->width / 2) - 1;

    int u1 = spr->u_off;
    int u2 = spr->u_off + spr->width;
    int v1 = spr->v_off;
    int v2 = spr->v_off + spr->height;

    if (spr->textureID != gCurrentTexture)
    {
        glBindTexture(GL_TEXTURE_2D, spr->textureID);
        gCurrentTexture = spr->textureID;
    }

    // left
    int x2l = x + su;
    glBegin(GL_QUADS);

        glTexCoord2i(u1, v1);
        glVertex3v16(x1, y1, g_depth);

        glTexCoord2i(u1, v2);
        glVertex2v16(x1, y2);

        glTexCoord2i(u1 + su, v2);
        glVertex2v16(x2l, y2);

        glTexCoord2i(u1 + su, v1);
        glVertex2v16(x2l, y1);

    glEnd();

    // center
    int x1l = x + su;
    x2l = x2 - su - 1;
    glBegin(GL_QUADS);

        glTexCoord2i(u1 + su, v1);
        glVertex2v16(x1l, y1);

        glTexCoord2i(u1 + su, v2);
        glVertex2v16(x1l, y2);

        glTexCoord2i(u1 + su, v2);
        glVertex2v16(x2l, y2);

        glTexCoord2i(u1 + su, v1);
        glVertex2v16(x2l, y1);

    glEnd();

    // right
    x1l = x2 - su - 1;
    glBegin(GL_QUADS);

        glTexCoord2i(u1 + su, v1);
        glVertex2v16(x1l, y1);

        glTexCoord2i(u1 + su, v2);
        glVertex2v16(x1l, y2);

        glTexCoord2i(u2, v2);
        glVertex2v16(x2, y2);

        glTexCoord2i(u2, v1);
        glVertex2v16(x2, y1);

    glEnd();

    g_depth++;
}

void glSpriteOnQuad(int x1, int y1, int x2, int y2, int x3, int y3, int x4, int y4,
                    int uoff, int voff, int flipmode, const glImage *spr)
{
    int u1 = spr->u_off + ((flipmode & GL_FLIP_H) ? spr->width - 1 : 0);
    int u2 = spr->u_off + ((flipmode & GL_FLIP_H) ? 0 : spr->width);
    int v1 = spr->v_off + ((flipmode & GL_FLIP_V) ? spr->height - 1 : 0);
    int v2 = spr->v_off + ((flipmode & GL_FLIP_V) ? 0 : spr->height);

    if (spr->textureID != gCurrentTexture)
    {
        glBindTexture(GL_TEXTURE_2D, spr->textureID);
        gCurrentTexture = spr->textureID;
    }

    glBegin(GL_QUADS);

        glTexCoord2i(u1 + uoff, v1 + voff);
        glVertex3v16(x1, y1, g_depth);
        glTexCoord2i(u1 + uoff, v2 + voff);
        glVertex2v16(x2, y2);
        glTexCoord2i(u2 + uoff, v2 + voff);
        glVertex2v16(x3, y3);
        glTexCoord2i(u2 + uoff, v1 + voff);
        glVertex2v16(x4, y4);

    glEnd();

    g_depth++;
}

int glLoadSpriteSet(glImage *sprite, const unsigned int numframes,
                    const uint16_t *texcoords, GL_TEXTURE_TYPE_ENUM type,
                    int sizeX, int sizeY, int param, int palette_width,
                    const void *palette, const void *texture)
{
    int textureID;
    glGenTextures(1, &textureID);
    glBindTexture(0, textureID);

    if (glTexImage2D(0, 0, type, sizeX, sizeY, 0, param, texture) == 0)
        return -1;

    bool needs_palette = !((type == GL_RGBA) || (type == GL_RGB));

    if (palette == NULL)
    {
        if (needs_palette)
            return -2;
    }
    else
    {
        if (!needs_palette)
            return -3;

        if (glColorTableEXT(0, 0, palette_width, 0, 0, palette) == 0)
            return -4;
    }

    // Init sprites texture coords and texture ID
    for (unsigned int i = 0; i < numframes; i++)
    {
        int j = i * 4; // Texcoords array is u_off, wid, hei
        sprite[i].textureID = textureID;
        sprite[i].u_off = texcoords[j];      // set x-coord
        sprite[i].v_off = texcoords[j + 1];  // y-coord

        // Don't decrease because NDS 3d core does not draw last vertical texel
        sprite[i].width = texcoords[j + 2];
        sprite[i].height = texcoords[j + 3]; // Ditto
    }

    return textureID;
}

int glLoadTileSet(glImage *sprite, int tile_wid, int tile_hei, int bmp_wid, int bmp_hei,
                  GL_TEXTURE_TYPE_ENUM type, int sizeX, int sizeY, int param,
                  int palette_width, const void *palette, const void *texture)
{
    int textureID;
    glGenTextures(1, &textureID);
    glBindTexture(0, textureID);

    if (glTexImage2D(0, 0, type, sizeX, sizeY, 0, param, texture) == 0)
        return -1;

    bool needs_palette = !((type == GL_RGBA) || (type == GL_RGB));

    if (palette == NULL)
    {
        if (needs_palette)
            return -2;
    }
    else
    {
        if (!needs_palette)
            return -3;

        if (glColorTableEXT(0, 0, palette_width, 0, 0, palette) == 0)
            return -4;
    }

    int i = 0;

    // Init sprites texture coords and texture ID
    for (int y = 0; y < (bmp_hei / tile_hei); y++)
    {
        for (int x = 0; x < (bmp_wid / tile_wid); x++)
        {
            sprite[i].width     = tile_wid;
            sprite[i].height    = tile_hei;
            sprite[i].u_off     = x * tile_wid;
            sprite[i].v_off     = y * tile_hei;
            sprite[i].textureID = textureID;
            i++;
        }
    }

    return textureID;
}
