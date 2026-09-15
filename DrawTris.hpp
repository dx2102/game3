#pragma once

#include <glm/glm.hpp>
#include <vector>

//DrawLines 的实心版：攒三角形，析构时一次画完
struct DrawTris {
	DrawTris(glm::mat4 const &world_to_clip);
	~DrawTris();

	void tri(glm::vec3 const &a, glm::vec3 const &b, glm::vec3 const &c, glm::u8vec4 const &color);
	void quad(glm::vec3 const &a, glm::vec3 const &b, glm::vec3 const &c, glm::vec3 const &d, glm::u8vec4 const &color);
	void rect(glm::vec2 const &min, glm::vec2 const &max, glm::u8vec4 const &color);

	glm::mat4 world_to_clip;
	struct Vertex {
		Vertex(glm::vec3 const &Position_, glm::u8vec4 const &Color_) : Position(Position_), Color(Color_) { }
		glm::vec3 Position;
		glm::u8vec4 Color;
	};
	std::vector< Vertex > attribs;
};
