#include "DrawTris.hpp"
#include "ColorProgram.hpp"
#include "gl_errors.hpp"

#include <glm/gtc/type_ptr.hpp>

static GLuint vertex_buffer = 0;
static GLuint vertex_array = 0;

static Load< void > setup_buffers(LoadTagDefault, [](){
	glGenBuffers(1, &vertex_buffer);
	glGenVertexArrays(1, &vertex_array);
	glBindVertexArray(vertex_array);
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glVertexAttribPointer(color_program->Position_vec4, 3, GL_FLOAT, GL_FALSE, sizeof(DrawTris::Vertex), (GLbyte *)0 + offsetof(DrawTris::Vertex, Position));
	glEnableVertexAttribArray(color_program->Position_vec4);
	glVertexAttribPointer(color_program->Color_vec4, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(DrawTris::Vertex), (GLbyte *)0 + offsetof(DrawTris::Vertex, Color));
	glEnableVertexAttribArray(color_program->Color_vec4);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);
	GL_ERRORS();
});

DrawTris::DrawTris(glm::mat4 const &world_to_clip_) : world_to_clip(world_to_clip_) { }

void DrawTris::tri(glm::vec3 const &a, glm::vec3 const &b, glm::vec3 const &c, glm::u8vec4 const &color) {
	attribs.emplace_back(a, color);
	attribs.emplace_back(b, color);
	attribs.emplace_back(c, color);
}

void DrawTris::quad(glm::vec3 const &a, glm::vec3 const &b, glm::vec3 const &c, glm::vec3 const &d, glm::u8vec4 const &color) {
	tri(a, b, c, color);
	tri(a, c, d, color);
}

void DrawTris::rect(glm::vec2 const &min, glm::vec2 const &max, glm::u8vec4 const &color) {
	quad(glm::vec3(min.x, min.y, 0.0f), glm::vec3(max.x, min.y, 0.0f), glm::vec3(max.x, max.y, 0.0f), glm::vec3(min.x, max.y, 0.0f), color);
}

DrawTris::~DrawTris() {
	if (attribs.empty()) return;
	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferData(GL_ARRAY_BUFFER, attribs.size() * sizeof(attribs[0]), attribs.data(), GL_STREAM_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glUseProgram(color_program->program);
	glUniformMatrix4fv(color_program->OBJECT_TO_CLIP_mat4, 1, GL_FALSE, glm::value_ptr(world_to_clip));
	glBindVertexArray(vertex_array);
	glDrawArrays(GL_TRIANGLES, 0, GLsizei(attribs.size()));
	glBindVertexArray(0);
	glUseProgram(0);
	GL_ERRORS();
}
