#include "engine/semantic_state.hpp"

#include <nlohmann/json.hpp>

namespace noemancer {
namespace {

using Json = nlohmann::json;

Json convention_to_json(const SemanticConvention& convention) {
    Json result = {
        {"name", convention.name},
        {"valueType", convention.value_type},
        {"description", convention.description},
        {"stability", convention.stability},
        {"sensitive", convention.sensitive}
    };
    if (!convention.unit.empty()) {
        result["unit"] = convention.unit;
    }
    if (!convention.coordinate_space.empty()) {
        result["coordinateSpace"] = convention.coordinate_space;
    }
    return result;
}

} // namespace

SemanticConventionRegistry::SemanticConventionRegistry()
    : conventions_{
          {
              "engine.entity.transform.position.x",
              "f32",
              "Entity translation on the world X axis.",
              "m",
              "world.right-handed.y-up",
              "experimental",
              false
          },
          {
              "engine.entity.transform.position.y",
              "f32",
              "Entity translation on the world Y axis.",
              "m",
              "world.right-handed.y-up",
              "experimental",
              false
          },
          {
              "engine.entity.transform.position.z",
              "f32",
              "Entity translation on the world Z axis.",
              "m",
              "world.right-handed.y-up",
              "experimental",
              false
          },
          {
              "engine.entity.transform.rotationEulerDegrees",
              "vector3<f32>",
              "Human-readable intrinsic XYZ rotation; runtime and physics normalize it to a quaternion.",
              "deg",
              "local.right-handed.y-up",
              "experimental",
              false
          },
          {
              "engine.entity.transform.rotationQuaternion",
              "quaternion<f32>",
              "Normalized runtime rotation in x/y/z/w order.",
              "",
              "local.right-handed.y-up",
              "experimental",
              false
          },
          {
              "engine.entity.velocity.linear.x",
              "f32",
              "Entity linear velocity on the world X axis.",
              "m/s",
              "world.right-handed.y-up",
              "experimental",
              false
          },
          {
              "engine.entity.velocity.linear.y",
              "f32",
              "Entity linear velocity on the world Y axis.",
              "m/s",
              "world.right-handed.y-up",
              "experimental",
              false
          },
          {
              "engine.entity.velocity.linear.z",
              "f32",
              "Entity linear velocity on the world Z axis.",
              "m/s",
              "world.right-handed.y-up",
              "experimental",
              false
          },
          {
              "engine.entity.velocity.angular.x",
              "f32",
              "Entity angular velocity around the world X axis.",
              "rad/s",
              "world.right-handed.y-up",
              "experimental",
              false
          },
          {
              "engine.entity.velocity.angular.y",
              "f32",
              "Entity angular velocity around the world Y axis.",
              "rad/s",
              "world.right-handed.y-up",
              "experimental",
              false
          },
          {
              "engine.entity.velocity.angular.z",
              "f32",
              "Entity angular velocity around the world Z axis.",
              "rad/s",
              "world.right-handed.y-up",
              "experimental",
              false
          }
      } {}

const std::vector<SemanticConvention>& SemanticConventionRegistry::conventions() const noexcept {
    return conventions_;
}

std::string SemanticConventionRegistry::schema_json() const {
    Json conventions = Json::array();
    for (const auto& convention : conventions_) {
        conventions.push_back(convention_to_json(convention));
    }

    const Json schema = {
        {"schemaVersion", "0.1"},
        {"registryId", "noemancer.semantic-conventions.core"},
        {"conventions", std::move(conventions)}
    };
    return schema.dump();
}

} // namespace noemancer
