/* Copyright (c) 2015-2024 The Khronos Group Inc.
 * Copyright (c) 2015-2024 Valve Corporation
 * Copyright (c) 2015-2024 LunarG, Inc.
 * Copyright (C) 2015-2024 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "stateless/stateless_validation.h"
#include "generated/layer_chassis_dispatch.h"

// Traits objects to allow string_join to operate on collections of const char *
template <typename String>
struct StringJoinSizeTrait {
    static size_t size(const String &str) { return str.size(); }
};

template <>
struct StringJoinSizeTrait<const char *> {
    static size_t size(const char *str) {
        if (!str) return 0;
        return strlen(str);
    }
};
// Similar to perl/python join
//    * String must support size, reserve, append, and be default constructable
//    * StringCollection must support size, const forward iteration, and store
//      strings compatible with String::append
//    * Accessor trait can be set if default accessors (compatible with string
//      and const char *) don't support size(StringCollection::value_type &)
//
// Return type based on sep type
template <typename String = std::string, typename StringCollection = std::vector<String>,
          typename Accessor = StringJoinSizeTrait<typename StringCollection::value_type>>
static inline String string_join(const String &sep, const StringCollection &strings) {
    String joined;
    const size_t count = strings.size();
    if (!count) return joined;

    // Prereserved storage, s.t. we will execute in linear time (avoids reallocation copies)
    size_t reserve = (count - 1) * sep.size();
    for (const auto &str : strings) {
        reserve += Accessor::size(str);  // abstracted to allow const char * type in StringCollection
    }
    joined.reserve(reserve + 1);

    // Seps only occur *between* strings entries, so first is special
    auto current = strings.cbegin();
    joined.append(*current);
    ++current;
    for (; current != strings.cend(); ++current) {
        joined.append(sep);
        joined.append(*current);
    }
    return joined;
}

// Requires StringCollection::value_type has a const char * constructor and is compatible the string_join::String above
template <typename StringCollection = std::vector<std::string>, typename SepString = std::string>
static inline SepString string_join(const char *sep, const StringCollection &strings) {
    return string_join<SepString, StringCollection>(SepString(sep), strings);
}

template <typename ExtensionState>
bool StatelessValidation::ValidateExtensionReqs(const ExtensionState &extensions, const char *vuid, const char *extension_type,
                                                vvl::Extension extension, const Location &extension_loc) const {
    bool skip = false;
    if (extension == vvl::Extension::Empty) {
        return skip;
    }
    auto info = ExtensionState::GetInfo(extension);

    if (!info.state) {
        return skip;  // Unknown extensions cannot be checked so report OK
    }

    // Check against the required list in the info
    std::vector<const char *> missing;
    for (const auto &req : info.requirements) {
        if (!(extensions.*(req.enabled))) {
            missing.push_back(req.name);
        }
    }

    // Report any missing requirements
    if (missing.size()) {
        std::string missing_joined_list = string_join(", ", missing);
        skip |= LogError(vuid, instance, extension_loc, "Missing extension%s required by the %s extension %s: %s.",
                         ((missing.size() > 1) ? "s" : ""), extension_type, String(extension), missing_joined_list.c_str());
    }
    return skip;
}

template <typename ExtensionState>
ExtEnabled ExtensionStateByName(const ExtensionState &extensions, vvl::Extension extension) {
    auto info = ExtensionState::GetInfo(extension);
    // unknown extensions can't be enabled in extension struct
    ExtEnabled state = info.state ? extensions.*(info.state) : kNotEnabled;
    return state;
}

bool StatelessValidation::manual_PreCallValidateCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                               const VkAllocationCallbacks *pAllocator, VkInstance *pInstance,
                                                               const ErrorObject &error_obj) const {
    bool skip = false;
    const Location create_info_loc = error_obj.location.dot(Field::pCreateInfo);
    // Note: From the spec--
    //  Providing a NULL VkInstanceCreateInfo::pApplicationInfo or providing an apiVersion of 0 is equivalent to providing
    //  an apiVersion of VK_MAKE_VERSION(1, 0, 0).  (a.k.a. VK_API_VERSION_1_0)
    uint32_t local_api_version = (pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->apiVersion)
                                     ? pCreateInfo->pApplicationInfo->apiVersion
                                     : VK_API_VERSION_1_0;

    uint32_t api_version_nopatch = VK_MAKE_VERSION(VK_VERSION_MAJOR(local_api_version), VK_VERSION_MINOR(local_api_version), 0);
    if (api_version != api_version_nopatch) {
        if ((api_version_nopatch < VK_API_VERSION_1_0) && (local_api_version != 0)) {
            skip |= LogError("VUID-VkApplicationInfo-apiVersion-04010", instance,
                             create_info_loc.dot(Field::pApplicationInfo).dot(Field::apiVersion),
                             "is (0x%08x). "
                             "Using VK_API_VERSION_%" PRIu32 "_%" PRIu32 ".",
                             local_api_version, api_version.Major(), api_version.Minor());
        } else {
            skip |= LogWarning(kVUIDUndefined, instance, create_info_loc.dot(Field::pApplicationInfo).dot(Field::apiVersion),
                               "is (0x%08x). "
                               "Assuming VK_API_VERSION_%" PRIu32 "_%" PRIu32 ".",
                               local_api_version, api_version.Major(), api_version.Minor());
        }
    }

    // Create and use a local instance extension object, as an actual instance has not been created yet
    uint32_t specified_version = (pCreateInfo->pApplicationInfo ? pCreateInfo->pApplicationInfo->apiVersion : VK_API_VERSION_1_0);
    InstanceExtensions local_instance_extensions;
    local_instance_extensions.InitFromInstanceCreateInfo(specified_version, pCreateInfo);

    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        vvl::Extension extension = GetExtension(pCreateInfo->ppEnabledExtensionNames[i]);
        skip |= ValidateExtensionReqs(local_instance_extensions, "VUID-vkCreateInstance-ppEnabledExtensionNames-01388", "instance",
                                      extension, create_info_loc.dot(Field::ppEnabledExtensionNames, i));
    }
    if (pCreateInfo->flags & VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR &&
        !local_instance_extensions.vk_khr_portability_enumeration) {
        skip |= LogError("VUID-VkInstanceCreateInfo-flags-06559", instance, create_info_loc.dot(Field::flags),
                         "has VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR set, but "
                         "ppEnabledExtensionNames does not include VK_KHR_portability_enumeration");
    }

    const auto *validation_features = vku::FindStructInPNextChain<VkValidationFeaturesEXT>(pCreateInfo->pNext);
    if (validation_features) {
        bool debug_printf = false;
        bool gpu_assisted = false;
        bool reserve_slot = false;
        for (uint32_t i = 0; i < validation_features->enabledValidationFeatureCount; i++) {
            switch (validation_features->pEnabledValidationFeatures[i]) {
                case VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT:
                    gpu_assisted = true;
                    break;

                case VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT:
                    debug_printf = true;
                    break;

                case VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT:
                    reserve_slot = true;
                    break;

                default:
                    break;
            }
        }
        if (reserve_slot && !gpu_assisted && !debug_printf) {
            skip |= LogError("VUID-VkValidationFeaturesEXT-pEnabledValidationFeatures-02967", instance,
                             create_info_loc.pNext(Struct::VkValidationFeaturesEXT, Field::pEnabledValidationFeatures),
                             "includes VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT but no "
                             "VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT or VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT.");
        }
        if (gpu_assisted && debug_printf) {
            skip |= LogError(
                "VUID-VkValidationFeaturesEXT-pEnabledValidationFeatures-02968", instance,
                create_info_loc.pNext(Struct::VkValidationFeaturesEXT, Field::pEnabledValidationFeatures),
                "includes both VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT and VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT.");
        }
    }

    const auto *debug_report_callback = vku::FindStructInPNextChain<VkDebugReportCallbackCreateInfoEXT>(pCreateInfo->pNext);
    if (debug_report_callback && !local_instance_extensions.vk_ext_debug_report) {
        skip |= LogError("VUID-VkInstanceCreateInfo-pNext-04925", instance, create_info_loc.dot(Field::ppEnabledExtensionNames),
                         "does not include VK_EXT_debug_report, but the pNext chain includes VkDebugReportCallbackCreateInfoEXT.");
    }
    const auto *debug_utils_messenger = vku::FindStructInPNextChain<VkDebugUtilsMessengerCreateInfoEXT>(pCreateInfo->pNext);
    if (debug_utils_messenger && !local_instance_extensions.vk_ext_debug_utils) {
        skip |= LogError("VUID-VkInstanceCreateInfo-pNext-04926", instance, create_info_loc.dot(Field::ppEnabledExtensionNames),
                         "does not include VK_EXT_debug_utils, but the pNext chain includes VkDebugUtilsMessengerCreateInfoEXT.");
    }
    const auto *direct_driver_loading_list = vku::FindStructInPNextChain<VkDirectDriverLoadingListLUNARG>(pCreateInfo->pNext);
    if (direct_driver_loading_list && !local_instance_extensions.vk_lunarg_direct_driver_loading) {
        skip |= LogError(
            "VUID-VkInstanceCreateInfo-pNext-09400", instance, create_info_loc.dot(Field::ppEnabledExtensionNames),
            "does not include VK_LUNARG_direct_driver_loading, but the pNext chain includes VkDirectDriverLoadingListLUNARG.");
    }

#ifdef VK_USE_PLATFORM_METAL_EXT
    auto export_metal_object_info = vku::FindStructInPNextChain<VkExportMetalObjectCreateInfoEXT>(pCreateInfo->pNext);
    while (export_metal_object_info) {
        if ((export_metal_object_info->exportObjectType != VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT) &&
            (export_metal_object_info->exportObjectType != VK_EXPORT_METAL_OBJECT_TYPE_METAL_COMMAND_QUEUE_BIT_EXT)) {
            skip |= LogError("VUID-VkInstanceCreateInfo-pNext-06779", instance, error_obj.location,
                             "The pNext chain contains a VkExportMetalObjectCreateInfoEXT whose "
                             "exportObjectType = %s, but only VkExportMetalObjectCreateInfoEXT structs with exportObjectType of "
                             "VK_EXPORT_METAL_OBJECT_TYPE_METAL_DEVICE_BIT_EXT or "
                             "VK_EXPORT_METAL_OBJECT_TYPE_METAL_COMMAND_QUEUE_BIT_EXT are allowed",
                             string_VkExportMetalObjectTypeFlagBitsEXT(export_metal_object_info->exportObjectType));
        }
        export_metal_object_info = vku::FindStructInPNextChain<VkExportMetalObjectCreateInfoEXT>(export_metal_object_info->pNext);
    }
#endif  // VK_USE_PLATFORM_METAL_EXT

    return skip;
}

void StatelessValidation::PostCallRecordCreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                                                       const VkAllocationCallbacks *pAllocator, VkInstance *pInstance,
                                                       const RecordObject &record_obj) {
    auto instance_data = GetLayerDataPtr(GetDispatchKey(*pInstance), layer_data_map);
    // Copy extension data into local object
    if (record_obj.result != VK_SUCCESS) return;
    this->instance_extensions = instance_data->instance_extensions;
    this->device_extensions = instance_data->device_extensions;
}

void StatelessValidation::CommonPostCallRecordEnumeratePhysicalDevice(const VkPhysicalDevice *phys_devices, const int count) {
    // Assume phys_devices is valid
    assert(phys_devices);
    for (int i = 0; i < count; ++i) {
        const auto &phys_device = phys_devices[i];
        if (0 == physical_device_properties_map.count(phys_device)) {
            auto phys_dev_props = new VkPhysicalDeviceProperties;
            DispatchGetPhysicalDeviceProperties(phys_device, phys_dev_props);
            physical_device_properties_map[phys_device] = phys_dev_props;

            // Enumerate the Device Ext Properties to save the PhysicalDevice supported extension state
            uint32_t ext_count = 0;
            vvl::unordered_set<vvl::Extension> dev_exts_enumerated{};
            std::vector<VkExtensionProperties> ext_props{};
            instance_dispatch_table.EnumerateDeviceExtensionProperties(phys_device, nullptr, &ext_count, nullptr);
            ext_props.resize(ext_count);
            instance_dispatch_table.EnumerateDeviceExtensionProperties(phys_device, nullptr, &ext_count, ext_props.data());
            for (uint32_t j = 0; j < ext_count; j++) {
                vvl::Extension extension = GetExtension(ext_props[j].extensionName);
                dev_exts_enumerated.insert(extension);

                if (extension == vvl::Extension::_VK_EXT_discard_rectangles) {
                    discard_rectangles_extension_version = ext_props[j].specVersion;
                } else if (extension == vvl::Extension::_VK_NV_scissor_exclusive) {
                    scissor_exclusive_extension_version = ext_props[j].specVersion;
                }
            }
            device_extensions_enumerated[phys_device] = std::move(dev_exts_enumerated);
        }
    }
}

void StatelessValidation::PostCallRecordEnumeratePhysicalDevices(VkInstance instance, uint32_t *pPhysicalDeviceCount,
                                                                 VkPhysicalDevice *pPhysicalDevices,
                                                                 const RecordObject &record_obj) {
    if ((VK_SUCCESS != record_obj.result) && (VK_INCOMPLETE != record_obj.result)) {
        return;
    }

    if (pPhysicalDeviceCount && pPhysicalDevices) {
        CommonPostCallRecordEnumeratePhysicalDevice(pPhysicalDevices, *pPhysicalDeviceCount);
    }
}

void StatelessValidation::PostCallRecordEnumeratePhysicalDeviceGroups(
    VkInstance instance, uint32_t *pPhysicalDeviceGroupCount, VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties,
    const RecordObject &record_obj) {
    if ((VK_SUCCESS != record_obj.result) && (VK_INCOMPLETE != record_obj.result)) {
        return;
    }

    if (pPhysicalDeviceGroupCount && pPhysicalDeviceGroupProperties) {
        for (uint32_t i = 0; i < *pPhysicalDeviceGroupCount; i++) {
            const auto &group = pPhysicalDeviceGroupProperties[i];
            CommonPostCallRecordEnumeratePhysicalDevice(group.physicalDevices, group.physicalDeviceCount);
        }
    }
}

void StatelessValidation::PreCallRecordDestroyInstance(VkInstance instance, const VkAllocationCallbacks *pAllocator,
                                                       const RecordObject &record_obj) {
    for (auto it = physical_device_properties_map.begin(); it != physical_device_properties_map.end();) {
        delete (it->second);
        it = physical_device_properties_map.erase(it);
    }
}

void StatelessValidation::PostCallRecordCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkDevice *pDevice,
                                                     const RecordObject &record_obj) {
    auto device_data = GetLayerDataPtr(GetDispatchKey(*pDevice), layer_data_map);
    if (record_obj.result != VK_SUCCESS) return;
    auto stateless_validation = device_data->GetValidationObject<StatelessValidation>();

    // Parmeter validation also uses extension data
    stateless_validation->device_extensions = this->device_extensions;

    VkPhysicalDeviceProperties device_properties = {};
    // Need to get instance and do a getlayerdata call...
    DispatchGetPhysicalDeviceProperties(physicalDevice, &device_properties);
    memcpy(&stateless_validation->device_limits, &device_properties.limits, sizeof(VkPhysicalDeviceLimits));

    if (IsExtEnabled(device_extensions.vk_nv_shading_rate_image)) {
        // Get the needed shading rate image limits
        VkPhysicalDeviceShadingRateImagePropertiesNV shading_rate_image_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&shading_rate_image_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.shading_rate_image_props = shading_rate_image_props;
    }

    if (IsExtEnabled(device_extensions.vk_nv_mesh_shader)) {
        // Get the needed mesh shader limits
        VkPhysicalDeviceMeshShaderPropertiesNV mesh_shader_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&mesh_shader_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.mesh_shader_props_nv = mesh_shader_props;
    }

    if (IsExtEnabled(device_extensions.vk_ext_mesh_shader)) {
        // Get the needed mesh shader EXT limits
        VkPhysicalDeviceMeshShaderPropertiesEXT mesh_shader_props_ext = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&mesh_shader_props_ext);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.mesh_shader_props_ext = mesh_shader_props_ext;
    }

    if (IsExtEnabled(device_extensions.vk_nv_ray_tracing)) {
        // Get the needed ray tracing limits
        VkPhysicalDeviceRayTracingPropertiesNV ray_tracing_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&ray_tracing_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.ray_tracing_props_nv = ray_tracing_props;
    }

    if (IsExtEnabled(device_extensions.vk_khr_ray_tracing_pipeline)) {
        // Get the needed ray tracing limits
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR ray_tracing_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&ray_tracing_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.ray_tracing_props_khr = ray_tracing_props;
    }

    if (IsExtEnabled(device_extensions.vk_khr_acceleration_structure)) {
        // Get the needed ray tracing acc structure limits
        VkPhysicalDeviceAccelerationStructurePropertiesKHR acc_structure_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&acc_structure_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.acc_structure_props = acc_structure_props;
    }

    if (IsExtEnabled(device_extensions.vk_ext_transform_feedback)) {
        // Get the needed transform feedback limits
        VkPhysicalDeviceTransformFeedbackPropertiesEXT transform_feedback_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&transform_feedback_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.transform_feedback_props = transform_feedback_props;
    }

    if (IsExtEnabled(device_extensions.vk_khr_vertex_attribute_divisor)) {
        // Get the needed vertex attribute divisor limits
        VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR vertex_attribute_divisor_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&vertex_attribute_divisor_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.vertex_attribute_divisor_props = vertex_attribute_divisor_props;
    } else if (IsExtEnabled(device_extensions.vk_ext_vertex_attribute_divisor)) {
        // Get the needed vertex attribute divisor limits
        VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT vertex_attribute_divisor_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&vertex_attribute_divisor_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.vertex_attribute_divisor_props = vku::InitStructHelper();
        phys_dev_ext_props.vertex_attribute_divisor_props.maxVertexAttribDivisor =
            vertex_attribute_divisor_props.maxVertexAttribDivisor;
    }

    if (IsExtEnabled(device_extensions.vk_khr_fragment_shading_rate)) {
        VkPhysicalDeviceFragmentShadingRatePropertiesKHR fragment_shading_rate_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&fragment_shading_rate_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.fragment_shading_rate_props = fragment_shading_rate_props;
    }

    if (IsExtEnabled(device_extensions.vk_khr_depth_stencil_resolve)) {
        VkPhysicalDeviceDepthStencilResolveProperties depth_stencil_resolve_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&depth_stencil_resolve_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.depth_stencil_resolve_props = depth_stencil_resolve_props;
    }

    if (IsExtEnabled(device_extensions.vk_ext_external_memory_host)) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT external_memory_host_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&external_memory_host_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.external_memory_host_props = external_memory_host_props;
    }

    if (IsExtEnabled(device_extensions.vk_ext_device_generated_commands)) {
        VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT device_generated_commands_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&device_generated_commands_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.device_generated_commands_props = device_generated_commands_props;
    }

     if (IsExtEnabled(device_extensions.vk_arm_render_pass_striped)) {
        VkPhysicalDeviceRenderPassStripedPropertiesARM renderpass_striped_props = vku::InitStructHelper();
        VkPhysicalDeviceProperties2 prop2 = vku::InitStructHelper(&renderpass_striped_props);
        DispatchGetPhysicalDeviceProperties2Helper(physicalDevice, &prop2);
        phys_dev_ext_props.renderpass_striped_props = renderpass_striped_props;
    }

    stateless_validation->phys_dev_ext_props = this->phys_dev_ext_props;

    GetEnabledDeviceFeatures(pCreateInfo, &stateless_validation->enabled_features, api_version);
}

bool StatelessValidation::manual_PreCallValidateCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *pCreateInfo,
                                                             const VkAllocationCallbacks *pAllocator, VkDevice *pDevice,
                                                             const ErrorObject &error_obj) const {
    bool skip = false;
    const Location create_info_loc = error_obj.location.dot(Field::pCreateInfo);
    for (size_t i = 0; i < pCreateInfo->enabledLayerCount; i++) {
        skip |= ValidateString(create_info_loc.dot(Field::ppEnabledLayerNames),
                               "VUID-VkDeviceCreateInfo-ppEnabledLayerNames-parameter", pCreateInfo->ppEnabledLayerNames[i]);
    }

    // If this device supports VK_KHR_portability_subset, it must be enabled
    const auto &exposed_extensions = device_extensions_enumerated.at(physicalDevice);
    const bool portability_supported =
        exposed_extensions.find(vvl::Extension::_VK_KHR_portability_subset) != exposed_extensions.end();
    bool portability_requested = false;
    bool fragmentmask_requested = false;

    vvl::unordered_set<vvl::Extension> enabled_extensions{};
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        vvl::Extension extension = GetExtension(pCreateInfo->ppEnabledExtensionNames[i]);
        enabled_extensions.insert(extension);
        skip |=
            ValidateString(create_info_loc.dot(Field::ppEnabledExtensionNames),
                           "VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-parameter", pCreateInfo->ppEnabledExtensionNames[i]);
        skip |= ValidateExtensionReqs(device_extensions, "VUID-vkCreateDevice-ppEnabledExtensionNames-01387", "device", extension,
                                      create_info_loc.dot(Field::ppEnabledExtensionNames, i));
        if (extension == vvl::Extension::_VK_KHR_portability_subset) {
            portability_requested = true;
        }
        if (extension == vvl::Extension::_VK_AMD_shader_fragment_mask) {
            fragmentmask_requested = true;
        }
    }

    if (portability_supported && !portability_requested) {
        skip |= LogError("VUID-VkDeviceCreateInfo-pProperties-04451", physicalDevice, error_obj.location,
                         "VK_KHR_portability_subset must be enabled because physical device %s supports it",
                         FormatHandle(physicalDevice).c_str());
    }

    {
        const bool maint1 = IsExtEnabledByCreateinfo(ExtensionStateByName(device_extensions, vvl::Extension::_VK_KHR_maintenance1));
        bool negative_viewport =
            IsExtEnabledByCreateinfo(ExtensionStateByName(device_extensions, vvl::Extension::_VK_AMD_negative_viewport_height));
        if (negative_viewport) {
            // Only need to check for VK_KHR_MAINTENANCE_1_EXTENSION_NAME if api version is 1.0, otherwise it's deprecated due to
            // integration into api version 1.1
            if (api_version >= VK_API_VERSION_1_1) {
                skip |= LogError("VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-01840", physicalDevice, error_obj.location,
                                 "ppEnabledExtensionNames must not include "
                                 "VK_AMD_negative_viewport_height if api version is greater than or equal to 1.1.");
            } else if (maint1) {
                skip |= LogError("VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-00374", physicalDevice, error_obj.location,
                                 "ppEnabledExtensionNames must not simultaneously include "
                                 "VK_KHR_maintenance1 and VK_AMD_negative_viewport_height.");
            }
        }
    }

    {
        const auto *descriptor_buffer_features = vku::FindStructInPNextChain<VkPhysicalDeviceDescriptorBufferFeaturesEXT>(pCreateInfo->pNext);
        if (descriptor_buffer_features && descriptor_buffer_features->descriptorBuffer && fragmentmask_requested) {
            skip |= LogError("VUID-VkDeviceCreateInfo-None-08095", physicalDevice, error_obj.location,
                             "If the descriptorBuffer feature is enabled, ppEnabledExtensionNames must not "
                             "contain VK_AMD_shader_fragment_mask.");
        }
    }

    {
        bool khr_bda =
            IsExtEnabledByCreateinfo(ExtensionStateByName(device_extensions, vvl::Extension::_VK_KHR_buffer_device_address));
        bool ext_bda =
            IsExtEnabledByCreateinfo(ExtensionStateByName(device_extensions, vvl::Extension::_VK_EXT_buffer_device_address));
        if (khr_bda && ext_bda) {
            skip |= LogError("VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-03328", physicalDevice, error_obj.location,
                             "ppEnabledExtensionNames must not contain both VK_KHR_buffer_device_address and "
                             "VK_EXT_buffer_device_address.");
        }
    }

    if (pCreateInfo->pNext != NULL && pCreateInfo->pEnabledFeatures) {
        // Check for get_physical_device_properties2 struct
        const auto *features2 = vku::FindStructInPNextChain<VkPhysicalDeviceFeatures2>(pCreateInfo->pNext);
        if (features2) {
            // Cannot include VkPhysicalDeviceFeatures2 and have non-null pEnabledFeatures
            skip |= LogError("VUID-VkDeviceCreateInfo-pNext-00373", physicalDevice, error_obj.location,
                             "pNext includes a VkPhysicalDeviceFeatures2 struct when "
                             "pCreateInfo->pEnabledFeatures is non-NULL.");
        }
    }

    auto features2 = vku::FindStructInPNextChain<VkPhysicalDeviceFeatures2>(pCreateInfo->pNext);
    const VkPhysicalDeviceFeatures *features = features2 ? &features2->features : pCreateInfo->pEnabledFeatures;
    const auto *robustness2_features = vku::FindStructInPNextChain<VkPhysicalDeviceRobustness2FeaturesEXT>(pCreateInfo->pNext);
    if (features && robustness2_features && robustness2_features->robustBufferAccess2 && !features->robustBufferAccess) {
        skip |= LogError("VUID-VkPhysicalDeviceRobustness2FeaturesEXT-robustBufferAccess2-04000", physicalDevice,
                         error_obj.location, "If robustBufferAccess2 is enabled then robustBufferAccess must be enabled.");
    }
    const auto *raytracing_features = vku::FindStructInPNextChain<VkPhysicalDeviceRayTracingPipelineFeaturesKHR>(pCreateInfo->pNext);
    if (raytracing_features && raytracing_features->rayTracingPipelineShaderGroupHandleCaptureReplayMixed &&
        !raytracing_features->rayTracingPipelineShaderGroupHandleCaptureReplay) {
        skip |= LogError(
            "VUID-VkPhysicalDeviceRayTracingPipelineFeaturesKHR-rayTracingPipelineShaderGroupHandleCaptureReplayMixed-03575",
            physicalDevice, error_obj.location,
            "If rayTracingPipelineShaderGroupHandleCaptureReplayMixed is VK_TRUE, "
            "rayTracingPipelineShaderGroupHandleCaptureReplay "
            "must also be VK_TRUE.");
    }

    const auto *vulkan_11_features = vku::FindStructInPNextChain<VkPhysicalDeviceVulkan11Features>(pCreateInfo->pNext);
    if (vulkan_11_features) {
        const VkBaseOutStructure *current = reinterpret_cast<const VkBaseOutStructure *>(pCreateInfo->pNext);
        while (current) {
            if (current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES) {
                skip |= LogError("VUID-VkDeviceCreateInfo-pNext-02829", physicalDevice, error_obj.location,
                                 "If the pNext chain includes a VkPhysicalDeviceVulkan11Features structure, then "
                                 "it must not include a %s structure",
                                 string_VkStructureType(current->sType));
                break;
            }
            current = reinterpret_cast<const VkBaseOutStructure *>(current->pNext);
        }

        // Check features are enabled if matching extension is passed in as well
        if (vulkan_11_features->shaderDrawParameters == VK_FALSE &&
            enabled_extensions.find(vvl::Extension::_VK_KHR_shader_draw_parameters) != enabled_extensions.end()) {
            skip |= LogError("VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-04476", physicalDevice, error_obj.location,
                             "%s is enabled but VkPhysicalDeviceVulkan11Features::shaderDrawParameters is not VK_TRUE.",
                             VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME);
        }
    }

    const auto *vulkan_12_features = vku::FindStructInPNextChain<VkPhysicalDeviceVulkan12Features>(pCreateInfo->pNext);
    if (vulkan_12_features) {
        const VkBaseOutStructure *current = reinterpret_cast<const VkBaseOutStructure *>(pCreateInfo->pNext);
        while (current) {
            if (current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES) {
                skip |= LogError("VUID-VkDeviceCreateInfo-pNext-02830", physicalDevice, error_obj.location,
                                 "If the pNext chain includes a VkPhysicalDeviceVulkan12Features structure, then it must not "
                                 "include a %s structure",
                                 string_VkStructureType(current->sType));
                break;
            }
            current = reinterpret_cast<const VkBaseOutStructure *>(current->pNext);
        }
        // Check features are enabled if matching extension is passed in as well
        if (vulkan_12_features->drawIndirectCount == VK_FALSE &&
            enabled_extensions.find(vvl::Extension::_VK_KHR_draw_indirect_count) != enabled_extensions.end()) {
            skip |= LogError("VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-02831", physicalDevice, error_obj.location,
                             "%s is enabled but VkPhysicalDeviceVulkan12Features::drawIndirectCount is not VK_TRUE.",
                             VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);
        }
        if (vulkan_12_features->samplerMirrorClampToEdge == VK_FALSE &&
            enabled_extensions.find(vvl::Extension::_VK_KHR_sampler_mirror_clamp_to_edge) != enabled_extensions.end()) {
            skip |= LogError("VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-02832", physicalDevice, error_obj.location,
                             " %s is enabled but VkPhysicalDeviceVulkan12Features::samplerMirrorClampToEdge "
                             "is not VK_TRUE.",
                             VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
        }
        if (vulkan_12_features->descriptorIndexing == VK_FALSE &&
            enabled_extensions.find(vvl::Extension::_VK_EXT_descriptor_indexing) != enabled_extensions.end()) {
            skip |= LogError("VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-02833", physicalDevice, error_obj.location,
                             "%s is enabled but VkPhysicalDeviceVulkan12Features::descriptorIndexing is not VK_TRUE.",
                             VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
        }
        if (vulkan_12_features->samplerFilterMinmax == VK_FALSE &&
            enabled_extensions.find(vvl::Extension::_VK_EXT_sampler_filter_minmax) != enabled_extensions.end()) {
            skip |= LogError("VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-02834", physicalDevice, error_obj.location,
                             "%s is enabled but VkPhysicalDeviceVulkan12Features::samplerFilterMinmax is not VK_TRUE.",
                             VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME);
        }

        if ((vulkan_12_features->shaderOutputViewportIndex == VK_FALSE || vulkan_12_features->shaderOutputLayer == VK_FALSE) &&
            enabled_extensions.find(vvl::Extension::_VK_EXT_shader_viewport_index_layer) != enabled_extensions.end()) {
            skip |= LogError("VUID-VkDeviceCreateInfo-ppEnabledExtensionNames-02835", physicalDevice, error_obj.location,
                             "%s is enabled but both VkPhysicalDeviceVulkan12Features::shaderOutputViewportIndex "
                             "and VkPhysicalDeviceVulkan12Features::shaderOutputLayer are not VK_TRUE.",
                             VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME);
        }

        if (vulkan_12_features->bufferDeviceAddress == VK_TRUE) {
            if (IsExtEnabledByCreateinfo(ExtensionStateByName(device_extensions, vvl::Extension::_VK_EXT_buffer_device_address))) {
                skip |= LogError("VUID-VkDeviceCreateInfo-pNext-04748", physicalDevice, error_obj.location,
                                 "pNext chain includes VkPhysicalDeviceVulkan12Features with bufferDeviceAddress "
                                 "set to VK_TRUE and ppEnabledExtensionNames contains VK_EXT_buffer_device_address");
            }
        }
    }

    const auto *vulkan_13_features = vku::FindStructInPNextChain<VkPhysicalDeviceVulkan13Features>(pCreateInfo->pNext);
    if (vulkan_13_features) {
        const VkBaseOutStructure *current = reinterpret_cast<const VkBaseOutStructure *>(pCreateInfo->pNext);
        while (current) {
            if (current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES ||
                current->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES) {
                skip |= LogError("VUID-VkDeviceCreateInfo-pNext-06532", physicalDevice, error_obj.location,
                                 "If the pNext chain includes a VkPhysicalDeviceVulkan13Features structure, then it must not "
                                 "include a %s structure",
                                 string_VkStructureType(current->sType));
                break;
            }
            current = reinterpret_cast<const VkBaseOutStructure *>(current->pNext);
        }
    }

    // Validate pCreateInfo->pQueueCreateInfos
    if (pCreateInfo->pQueueCreateInfos) {
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
            const VkDeviceQueueCreateInfo &queue_create_info = pCreateInfo->pQueueCreateInfos[i];
            const uint32_t requested_queue_family = queue_create_info.queueFamilyIndex;
            if (requested_queue_family == VK_QUEUE_FAMILY_IGNORED) {
                skip |=
                    LogError("VUID-VkDeviceQueueCreateInfo-queueFamilyIndex-00381", physicalDevice, error_obj.location,
                             "pCreateInfo->pQueueCreateInfos[%" PRIu32
                             "].queueFamilyIndex is VK_QUEUE_FAMILY_IGNORED, but it is required to provide a valid queue family "
                             "index value.",
                             i);
            }

            if (queue_create_info.pQueuePriorities != nullptr) {
                for (uint32_t j = 0; j < queue_create_info.queueCount; ++j) {
                    const float queue_priority = queue_create_info.pQueuePriorities[j];
                    if (!(queue_priority >= 0.f) || !(queue_priority <= 1.f)) {
                        skip |= LogError("VUID-VkDeviceQueueCreateInfo-pQueuePriorities-00383", physicalDevice, error_obj.location,
                                         "pCreateInfo->pQueueCreateInfos[%" PRIu32 "].pQueuePriorities[%" PRIu32
                                         "] (=%f) is not between 0 and 1 (inclusive).",
                                         i, j, queue_priority);
                    }
                }
            }

            // Need to know if protectedMemory feature is passed in preCall to creating the device
            VkBool32 protected_memory = VK_FALSE;
            const VkPhysicalDeviceProtectedMemoryFeatures *protected_features =
                vku::FindStructInPNextChain<VkPhysicalDeviceProtectedMemoryFeatures>(pCreateInfo->pNext);
            if (protected_features) {
                protected_memory = protected_features->protectedMemory;
            } else if (vulkan_11_features) {
                protected_memory = vulkan_11_features->protectedMemory;
            }
            if (((queue_create_info.flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT) != 0) && (protected_memory == VK_FALSE)) {
                skip |= LogError("VUID-VkDeviceQueueCreateInfo-flags-02861", physicalDevice, error_obj.location,
                                 "pCreateInfo->flags contains VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT without the "
                                 "protectedMemory feature being enabled as well.");
            }
        }
    }

    // feature dependencies for VK_KHR_variable_pointers
    const auto *variable_pointers_features = vku::FindStructInPNextChain<VkPhysicalDeviceVariablePointersFeatures>(pCreateInfo->pNext);
    VkBool32 variable_pointers = VK_FALSE;
    VkBool32 variable_pointers_storage_buffer = VK_FALSE;
    if (vulkan_11_features) {
        variable_pointers = vulkan_11_features->variablePointers;
        variable_pointers_storage_buffer = vulkan_11_features->variablePointersStorageBuffer;
    } else if (variable_pointers_features) {
        variable_pointers = variable_pointers_features->variablePointers;
        variable_pointers_storage_buffer = variable_pointers_features->variablePointersStorageBuffer;
    }
    if ((variable_pointers == VK_TRUE) && (variable_pointers_storage_buffer == VK_FALSE)) {
        skip |= LogError("VUID-VkPhysicalDeviceVariablePointersFeatures-variablePointers-01431", physicalDevice, error_obj.location,
                         "If variablePointers is VK_TRUE then variablePointersStorageBuffer also needs to be VK_TRUE");
    }

    // feature dependencies for VK_KHR_multiview
    const auto *multiview_features = vku::FindStructInPNextChain<VkPhysicalDeviceMultiviewFeatures>(pCreateInfo->pNext);
    VkBool32 multiview = VK_FALSE;
    VkBool32 multiview_geometry_shader = VK_FALSE;
    VkBool32 multiview_tessellation_shader = VK_FALSE;
    if (vulkan_11_features) {
        multiview = vulkan_11_features->multiview;
        multiview_geometry_shader = vulkan_11_features->multiviewGeometryShader;
        multiview_tessellation_shader = vulkan_11_features->multiviewTessellationShader;
    } else if (multiview_features) {
        multiview = multiview_features->multiview;
        multiview_geometry_shader = multiview_features->multiviewGeometryShader;
        multiview_tessellation_shader = multiview_features->multiviewTessellationShader;
    }
    if ((multiview == VK_FALSE) && (multiview_geometry_shader == VK_TRUE)) {
        skip |= LogError("VUID-VkPhysicalDeviceMultiviewFeatures-multiviewGeometryShader-00580", physicalDevice, error_obj.location,
                         "If multiviewGeometryShader is VK_TRUE then multiview also needs to be VK_TRUE");
    }
    if ((multiview == VK_FALSE) && (multiview_tessellation_shader == VK_TRUE)) {
        skip |= LogError("VUID-VkPhysicalDeviceMultiviewFeatures-multiviewTessellationShader-00581", physicalDevice,
                         error_obj.location, "If multiviewTessellationShader is VK_TRUE then multiview also needs to be VK_TRUE");
    }
    const auto *fsr_features = vku::FindStructInPNextChain<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>(pCreateInfo->pNext);
    const auto *mesh_shader_features = vku::FindStructInPNextChain<VkPhysicalDeviceMeshShaderFeaturesEXT>(pCreateInfo->pNext);
    if (mesh_shader_features) {
        if ((multiview == VK_FALSE) && (mesh_shader_features->multiviewMeshShader)) {
            skip |= LogError("VUID-VkPhysicalDeviceMeshShaderFeaturesEXT-multiviewMeshShader-07032", physicalDevice,
                             error_obj.location, "If multiviewMeshShader is VK_TRUE then multiview also needs to be VK_TRUE");
        }
        if ((!fsr_features || !fsr_features->primitiveFragmentShadingRate) &&
            (mesh_shader_features->primitiveFragmentShadingRateMeshShader)) {
            skip |= LogError(
                "VUID-VkPhysicalDeviceMeshShaderFeaturesEXT-primitiveFragmentShadingRateMeshShader-07033", physicalDevice,
                error_obj.location,
                "If primitiveFragmentShadingRateMeshShader is VK_TRUE then primitiveFragmentShadingRate also needs to be VK_TRUE");
        }
    }

    return skip;
}

bool StatelessValidation::manual_PreCallValidateGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice physicalDevice, const VkPhysicalDeviceImageFormatInfo2 *pImageFormatInfo,
    VkImageFormatProperties2 *pImageFormatProperties, const ErrorObject &error_obj) const {
    bool skip = false;

    if (pImageFormatInfo != nullptr) {
        const Location format_info_loc = error_obj.location.dot(Field::pImageFormatInfo);
        const auto image_stencil_struct = vku::FindStructInPNextChain<VkImageStencilUsageCreateInfo>(pImageFormatInfo->pNext);
        if (image_stencil_struct != nullptr) {
            if ((image_stencil_struct->stencilUsage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) != 0) {
                VkImageUsageFlags legal_flags = (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
                // No flags other than the legal attachment bits may be set
                legal_flags |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
                if ((image_stencil_struct->stencilUsage & ~legal_flags) != 0) {
                    skip |= LogError("VUID-VkImageStencilUsageCreateInfo-stencilUsage-02539", physicalDevice,
                                     format_info_loc.pNext(Struct::VkImageStencilUsageCreateInfo, Field::stencilUsage), "is %s.",
                                     string_VkImageUsageFlags(image_stencil_struct->stencilUsage).c_str());
                }
            }
        }
        const auto image_drm_format = vku::FindStructInPNextChain<VkPhysicalDeviceImageDrmFormatModifierInfoEXT>(pImageFormatInfo->pNext);
        if (image_drm_format) {
            if (pImageFormatInfo->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
                skip |= LogError("VUID-VkPhysicalDeviceImageFormatInfo2-tiling-02249", physicalDevice,
                                 format_info_loc.dot(Field::tiling),
                                 "(%s) but no VkPhysicalDeviceImageDrmFormatModifierInfoEXT in pNext chain.",
                                 string_VkImageTiling(pImageFormatInfo->tiling));
            }
            if (image_drm_format->sharingMode == VK_SHARING_MODE_CONCURRENT) {
                if (image_drm_format->queueFamilyIndexCount <= 1) {
                    skip |=
                        LogError("VUID-VkPhysicalDeviceImageDrmFormatModifierInfoEXT-sharingMode-02315", physicalDevice,
                                 format_info_loc.pNext(Struct::VkPhysicalDeviceImageDrmFormatModifierInfoEXT, Field::sharingMode),
                                 "is VK_SHARING_MODE_CONCURRENT, but queueFamilyIndexCount is %" PRIu32 ".",
                                 image_drm_format->queueFamilyIndexCount);
                } else if (!image_drm_format->pQueueFamilyIndices) {
                    skip |= LogError(
                        "VUID-VkPhysicalDeviceImageDrmFormatModifierInfoEXT-sharingMode-02314", physicalDevice,
                        format_info_loc.pNext(Struct::VkPhysicalDeviceImageDrmFormatModifierInfoEXT, Field::sharingMode),
                        "is VK_SHARING_MODE_CONCURRENT, queueFamilyIndexCount is %" PRIu32 ", but pQueueFamilyIndices is NULL.",
                        image_drm_format->queueFamilyIndexCount);
                } else {
                    uint32_t queue_family_property_count = 0;
                    DispatchGetPhysicalDeviceQueueFamilyProperties2Helper(physicalDevice, &queue_family_property_count, nullptr);
                    vvl::unordered_set<uint32_t> queue_family_indices_set;
                    for (uint32_t i = 0; i < image_drm_format->queueFamilyIndexCount; i++) {
                        const uint32_t queue_index = image_drm_format->pQueueFamilyIndices[i];
                        if (queue_family_indices_set.find(queue_index) != queue_family_indices_set.end()) {
                            skip |= LogError("VUID-VkPhysicalDeviceImageDrmFormatModifierInfoEXT-sharingMode-02316", physicalDevice,
                                             format_info_loc.pNext(Struct::VkPhysicalDeviceImageDrmFormatModifierInfoEXT,
                                                                   Field::pQueueFamilyIndices, i),
                                             "is %" PRIu32 ", but is duplicated in pQueueFamilyIndices.", queue_index);
                            break;
                        } else if (queue_index >= queue_family_property_count) {
                            skip |= LogError(
                                "VUID-VkPhysicalDeviceImageDrmFormatModifierInfoEXT-sharingMode-02316", physicalDevice,
                                format_info_loc.pNext(Struct::VkPhysicalDeviceImageDrmFormatModifierInfoEXT,
                                                      Field::pQueueFamilyIndices, i),
                                "is %" PRIu32
                                ", but vkGetPhysicalDeviceQueueFamilyProperties2::pQueueFamilyPropertyCount returned %" PRIu32 ".",
                                queue_index, queue_family_property_count);
                        }
                        queue_family_indices_set.emplace(queue_index);
                    }
                }
            }
        } else {
            if (pImageFormatInfo->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
                skip |= LogError("VUID-VkPhysicalDeviceImageFormatInfo2-tiling-02249", physicalDevice,
                                 format_info_loc.dot(Field::tiling),
                                 "is VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, but pNext chain not include "
                                 "VkPhysicalDeviceImageDrmFormatModifierInfoEXT.");
            }
        }
        if (pImageFormatInfo->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT &&
            (pImageFormatInfo->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) {
            const auto format_list = vku::FindStructInPNextChain<VkImageFormatListCreateInfo>(pImageFormatInfo->pNext);
            if (!format_list || format_list->viewFormatCount == 0) {
                skip |= LogError(
                    "VUID-VkPhysicalDeviceImageFormatInfo2-tiling-02313", physicalDevice, format_info_loc,
                    "tiling is VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT and flags contain VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT "
                    "bit, but the pNext chain does not include VkImageFormatListCreateInfo with non-zero viewFormatCount.");
            }
        }
    }

    return skip;
}

bool StatelessValidation::manual_PreCallValidateGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice physicalDevice, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
    VkImageCreateFlags flags, VkImageFormatProperties *pImageFormatProperties, const ErrorObject &error_obj) const {
    bool skip = false;

    if (tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
        skip |= LogError("VUID-vkGetPhysicalDeviceImageFormatProperties-tiling-02248", physicalDevice,
                         error_obj.location.dot(Field::tiling), "is VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT.");
    }

    return skip;
}

bool StatelessValidation::manual_PreCallValidateSetDebugUtilsObjectNameEXT(VkDevice device,
                                                                           const VkDebugUtilsObjectNameInfoEXT *pNameInfo,
                                                                           const ErrorObject &error_obj) const {
    bool skip = false;
    const Location name_info_loc = error_obj.location.dot(Field::pNameInfo);
    if (pNameInfo->objectType == VK_OBJECT_TYPE_UNKNOWN) {
        skip |= LogError("VUID-vkSetDebugUtilsObjectNameEXT-pNameInfo-02587", device, name_info_loc.dot(Field::objectType),
                         "cannot be VK_OBJECT_TYPE_UNKNOWN.");
    }
    if (pNameInfo->objectHandle == HandleToUint64(VK_NULL_HANDLE)) {
        skip |= LogError("VUID-vkSetDebugUtilsObjectNameEXT-pNameInfo-02588", device, name_info_loc.dot(Field::objectHandle),
                         "cannot be VK_NULL_HANDLE.");
    }

    if ((pNameInfo->objectType == VK_OBJECT_TYPE_UNKNOWN) && (pNameInfo->objectHandle == HandleToUint64(VK_NULL_HANDLE))) {
        skip |= LogError("VUID-VkDebugUtilsObjectNameInfoEXT-objectType-02589", device, name_info_loc.dot(Field::objectType),
                         "is VK_OBJECT_TYPE_UNKNOWN but objectHandle is VK_NULL_HANDLE");
    }

    return skip;
}

bool StatelessValidation::manual_PreCallValidateSetDebugUtilsObjectTagEXT(VkDevice device,
                                                                          const VkDebugUtilsObjectTagInfoEXT *pTagInfo,
                                                                          const ErrorObject &error_obj) const {
    bool skip = false;
    if (pTagInfo->objectType == VK_OBJECT_TYPE_UNKNOWN) {
        skip |= LogError("VUID-VkDebugUtilsObjectTagInfoEXT-objectType-01908", device, error_obj.location,
                         "pTagInfo->objectType cannot be VK_OBJECT_TYPE_UNKNOWN.");
    }
    return skip;
}

bool StatelessValidation::manual_PreCallValidateGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                                                             VkPhysicalDeviceProperties2 *pProperties,
                                                                             const ErrorObject &error_obj) const {
    bool skip = false;
    const auto *api_props_lists = vku::FindStructInPNextChain<VkPhysicalDeviceLayeredApiPropertiesListKHR>(pProperties->pNext);
    if (api_props_lists && api_props_lists->pLayeredApis) {
        for (uint32_t i = 0; i < api_props_lists->layeredApiCount; i++) {
            if (const auto *api_vulkan_props = vku::FindStructInPNextChain<VkPhysicalDeviceLayeredApiVulkanPropertiesKHR>(
                    api_props_lists->pLayeredApis[i].pNext)) {
                const VkBaseOutStructure *current = static_cast<const VkBaseOutStructure *>(api_vulkan_props->properties.pNext);
                while (current) {
                    // only VkPhysicalDeviceDriverProperties and VkPhysicalDeviceIDProperties allowed
                    if (current->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES &&
                        current->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES) {
                        skip |= LogError("VUID-VkPhysicalDeviceLayeredApiVulkanPropertiesKHR-pNext-10011", physicalDevice,
                                         error_obj.location.dot(Field::pProperties)
                                             .pNext(Struct::VkPhysicalDeviceLayeredApiPropertiesListKHR, Field::pLayeredApis, i)
                                             .dot(Field::properties)
                                             .dot(Field::pNext),
                                         "contains an invalid struct (%s).", string_VkStructureType(current->sType));
                    }
                    current = current->pNext;
                }
            }
        }
    }
    return skip;
}