// Minimal GLES2 declarations, just enough to type-check the player offline.
#pragma once
typedef unsigned int GLenum; typedef unsigned int GLuint; typedef int GLint;
typedef int GLsizei; typedef float GLfloat; typedef unsigned char GLboolean;
typedef char GLchar; typedef void GLvoid;
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_ARRAY_BUFFER 0x8892
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#ifdef __cplusplus
extern "C" {
#endif
GLuint glCreateShader(GLenum); void glShaderSource(GLuint, int, const char* const*, const GLint*);
void glCompileShader(GLuint); void glGetShaderiv(GLuint, GLenum, GLint*);
void glGetShaderInfoLog(GLuint, GLsizei, GLsizei*, GLchar*); void glDeleteShader(GLuint);
GLuint glCreateProgram(void); void glAttachShader(GLuint, GLuint); void glLinkProgram(GLuint);
void glGetProgramiv(GLuint, GLenum, GLint*); void glGetProgramInfoLog(GLuint, GLsizei, GLsizei*, GLchar*);
GLint glGetAttribLocation(GLuint, const GLchar*); GLint glGetUniformLocation(GLuint, const GLchar*);
void glGenBuffers(GLsizei, GLuint*); void glBindBuffer(GLenum, GLuint);
void glBufferData(GLenum, long, const void*, GLenum);
void glGenTextures(GLsizei, GLuint*); void glBindTexture(GLenum, GLuint);
void glTexParameteri(GLenum, GLenum, GLint);
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
void glTexSubImage2D(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
void glViewport(GLint, GLint, GLsizei, GLsizei); void glClearColor(GLfloat, GLfloat, GLfloat, GLfloat);
void glClear(GLuint); void glUseProgram(GLuint); void glUniform4f(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
void glUniform1i(GLint, GLint); void glActiveTexture(GLenum);
void glEnableVertexAttribArray(GLuint); void glDisableVertexAttribArray(GLuint);
void glVertexAttribPointer(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
void glDrawArrays(GLenum, GLint, GLsizei);
#ifdef __cplusplus
}
#endif
