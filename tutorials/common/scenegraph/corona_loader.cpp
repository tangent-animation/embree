// ======================================================================== //
// Copyright 2009-2016 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "corona_loader.h"
#include "xml_parser.h"
#include "obj_loader.h"

namespace embree
{
  class CoronaLoader
  {
  public:

    static Ref<SceneGraph::Node> load(const FileName& fileName, const AffineSpace3fa& space);
    CoronaLoader(const FileName& fileName, const AffineSpace3fa& space);

  private:
    template<typename T> T load(const Ref<XML>& xml) { assert(false); return T(zero); }
    Ref<SceneGraph::MaterialNode> loadMaterial(const Ref<XML>& xml);
    void  loadMaterialDefinition(const Ref<XML>& xml);
    void  loadMapDefinition(const Ref<XML>& xml);
    Ref<SceneGraph::Node> loadMaterialLibrary(const FileName& fileName);
    Ref<SceneGraph::Node> loadObject(const Ref<XML>& xml);
    std::tuple<Ref<SceneGraph::MaterialNode>, avector<AffineSpace3fa> > loadInstances(const Ref<XML>& xml);
    Ref<SceneGraph::Node> loadGroupNode(const Ref<XML>& xml);
    Ref<SceneGraph::Node> loadNode(const Ref<XML>& xml);

  private:
    FileName path; 
    std::map<std::string,Ref<SceneGraph::MaterialNode> > materialMap; 
  public:
    Ref<SceneGraph::Node> root;
  };

  template<> FileName CoronaLoader::load<FileName>(const Ref<XML>& xml) 
  {
    if (xml->body.size() != 1) THROW_RUNTIME_ERROR(xml->loc.str()+": wrong FileName body");
    return xml->body[0].Identifier();
  }

  template<> std::string CoronaLoader::load<std::string>(const Ref<XML>& xml) 
  {
    if (xml->body.size() != 1) THROW_RUNTIME_ERROR(xml->loc.str()+": wrong string body");
    return xml->body[0].Identifier();
  }

  template<> int CoronaLoader::load<int>(const Ref<XML>& xml) {
    if (xml->body.size() < 1) THROW_RUNTIME_ERROR(xml->loc.str()+": wrong int body");
    return xml->body[0].Int();
  }

  template<> float CoronaLoader::load<float>(const Ref<XML>& xml) {
    if (xml->body.size() < 1) THROW_RUNTIME_ERROR(xml->loc.str()+": wrong float body");
    return xml->body[0].Float();
  }

  template<> Vec3f CoronaLoader::load<Vec3f>(const Ref<XML>& xml) {
    if (xml->body.size() < 3) THROW_RUNTIME_ERROR(xml->loc.str()+": wrong float3 body");
    return Vec3f(xml->body[0].Float(),xml->body[1].Float(),xml->body[2].Float());
  }

  template<> Vec3fa CoronaLoader::load<Vec3fa>(const Ref<XML>& xml) {
    if (xml->body.size() < 3) THROW_RUNTIME_ERROR(xml->loc.str()+": wrong float3 body");
    return Vec3fa(xml->body[0].Float(),xml->body[1].Float(),xml->body[2].Float());
  }

  template<> AffineSpace3fa CoronaLoader::load<AffineSpace3fa>(const Ref<XML>& xml) 
  {
    if (xml->body.size() != 12) THROW_RUNTIME_ERROR(xml->loc.str()+": wrong AffineSpace body");
    return AffineSpace3fa(LinearSpace3fa(xml->body[0].Float(),xml->body[1].Float(),xml->body[ 2].Float(),
                                         xml->body[4].Float(),xml->body[5].Float(),xml->body[ 6].Float(),
                                         xml->body[8].Float(),xml->body[9].Float(),xml->body[10].Float()),
                          Vec3fa(xml->body[3].Float(),xml->body[7].Float(),xml->body[11].Float()));
  }

  Ref<SceneGraph::MaterialNode> CoronaLoader::loadMaterial(const Ref<XML>& xml) 
  {
    if (xml->name != "material") 
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid material: "+xml->name);

    /* native material */
    if (xml->parm("class") == "Native") 
    {
      /* we convert into an OBJ material */
      OBJMaterial objmaterial;
      for (auto child : xml->children)
      {
        if (child->name == "diffuse") 
          objmaterial.Kd = load<Vec3fa>(child);
        else if (child->name == "reflect") {
          objmaterial.Ks = load<Vec3fa>(child->child("color"));
          objmaterial.Ni = load<float >(child->child("ior"));
          objmaterial.Ns = load<float >(child->child("glossiness"));
        }
        else if (child->name == "translucency") {
          objmaterial.Kt = load<Vec3fa>(child->child("color"));
        }
      }
      
      /* return material */
      Material material; new (&material) OBJMaterial(objmaterial);
      return new SceneGraph::MaterialNode(material);
    }

    /* reference by name */
    else if (xml->parm("class") == "Reference") 
    {
      const std::string name = load<std::string>(xml);
      return materialMap[name];
    }

    /* else return default material */
    else 
    {
      Material objmtl; new (&objmtl) OBJMaterial;
      return new SceneGraph::MaterialNode(objmtl);
    }
  }

  void CoronaLoader::loadMaterialDefinition(const Ref<XML>& xml) 
  {
    if (xml->name != "materialDefinition") 
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid material definition: "+xml->name);
    if (xml->children.size() != 1) 
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid material definition");

    const std::string name = xml->parm("name");
    materialMap[name] = loadMaterial(xml->children[0]);
  }

  void CoronaLoader::loadMapDefinition(const Ref<XML>& xml) 
  {
    if (xml->name != "mapDefinition") 
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid map definition: "+xml->name);
    if (xml->children.size() != 1) 
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid map definition");

    //const std::string name = xml->parm("name");
    //textureMap[name] = loadMap(xml->children[0]);
  }

  Ref<SceneGraph::Node> CoronaLoader::loadMaterialLibrary(const FileName& fileName) 
  {
    Ref<XML> xml = parseXML(path+fileName,"/.-",false);
    if (xml->name != "mtlLib") 
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid material library");
    
    for (auto child : xml->children)
    {
      if (child->name == "materialDefinition")
        loadMaterialDefinition(child);
      else if (child->name == "mapDefinition")
        loadMapDefinition(child);
    }

    return nullptr;
  }

  Ref<SceneGraph::Node> CoronaLoader::loadObject(const Ref<XML>& xml) 
  {
    if (xml->name != "object") 
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid object node");
    if (xml->parm("class") != "file")
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid object class");
    const FileName fileName = load<FileName>(xml);
    return SceneGraph::load(path+fileName);
  }

  std::tuple<Ref<SceneGraph::MaterialNode>, avector<AffineSpace3fa> > CoronaLoader::loadInstances(const Ref<XML>& xml) 
  {
    if (xml->name != "instance") 
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid instance node");

    /* create default material */
    Material objmtl; new (&objmtl) OBJMaterial;
    Ref<SceneGraph::MaterialNode> material = new SceneGraph::MaterialNode(objmtl);

    avector<AffineSpace3fa> xfms;
    for (size_t i=0; i<xml->children.size(); i++)
    {
      Ref<XML> child = xml->children[i];
      if      (child->name == "material" ) material = loadMaterial(child);
      else if (child->name == "transform") xfms.push_back(load<AffineSpace3fa>(child));
      else THROW_RUNTIME_ERROR(child->loc.str()+": unknown node: "+child->name);
    }

    return std::make_tuple(material,xfms);
  }

  Ref<SceneGraph::Node> CoronaLoader::loadGroupNode(const Ref<XML>& xml) 
  {
    if (xml->children.size() < 1) 
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid group node");

    /* load instances */
    Ref<SceneGraph::MaterialNode> material;
    avector<AffineSpace3fa> xfms;
    std::tie(material,xfms) = loadInstances(xml->children[0]);
    
    /* load meshes */
    Ref<SceneGraph::GroupNode> objects = new SceneGraph::GroupNode;
    for (size_t i=1; i<xml->children.size(); i++)
      objects->add(loadObject(xml->children[i]));
    
    /* force material */
    objects->setMaterial(material);

    /* create instances */
    Ref<SceneGraph::GroupNode> instances = new SceneGraph::GroupNode;
    for (size_t i=0; i<xfms.size(); i++) 
      instances->add(new SceneGraph::TransformNode(xfms[i],objects.cast<SceneGraph::Node>()));

    return instances.cast<SceneGraph::Node>();
  }
  
  Ref<SceneGraph::Node> CoronaLoader::loadNode(const Ref<XML>& xml)
  {
    if      (xml->name == "conffile"     ) return nullptr;
    else if (xml->name == "mtllib"       ) return loadMaterialLibrary(load<FileName>(xml));
    else if (xml->name == "camera"       ) return nullptr;
    else if (xml->name == "environment"  ) return nullptr;
    else if (xml->name == "geometryGroup") return loadGroupNode(xml);
    else THROW_RUNTIME_ERROR(xml->loc.str()+": unknown tag: "+xml->name);
    return nullptr;
  }

  Ref<SceneGraph::Node> CoronaLoader::load(const FileName& fileName, const AffineSpace3fa& space) {
    CoronaLoader loader(fileName,space); return loader.root;
  }

  CoronaLoader::CoronaLoader(const FileName& fileName, const AffineSpace3fa& space)
  {
    path = fileName.path();
    Ref<XML> xml = parseXML(fileName,"/.-",false);
    if (xml->name == "scene") 
    {
      Ref<SceneGraph::GroupNode> group = new SceneGraph::GroupNode;
      for (size_t i=0; i<xml->children.size(); i++) { 
        group->add(loadNode(xml->children[i]));
      }
      root = group.cast<SceneGraph::Node>();
    }
    else 
      THROW_RUNTIME_ERROR(xml->loc.str()+": invalid scene tag");

    if (space == AffineSpace3fa(one)) 
      return;
    
    root = new SceneGraph::TransformNode(space,root);
  }

  /*! read from disk */
  Ref<SceneGraph::Node> loadCorona(const FileName& fileName, const AffineSpace3fa& space) {
    return CoronaLoader::load(fileName,space);
  }
}
