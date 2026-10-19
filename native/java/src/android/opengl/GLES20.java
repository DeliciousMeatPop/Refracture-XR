package android.opengl;

/**
 * The GL queries games make from Java while picking a renderer (see EGL14). Answers as a Quest 2's Adreno 650;
 * the other GLES20 methods stay unimplemented natives, since Refract Native renders with Vulkan only.
 */
public class GLES20 {
    private static final int GL_VENDOR = 0x1F00;
    private static final int GL_RENDERER = 0x1F01;
    private static final int GL_VERSION = 0x1F02;
    private static final int GL_EXTENSIONS = 0x1F03;
    private static final int GL_SHADING_LANGUAGE_VERSION = 0x8B8C;

    /**
     * An Adreno 650's extensions (the common subset). UE4 picks its texture formats from these: without
     * GL_KHR_texture_compression_astc_ldr an ASTC-cooked game gets placeholder textures. ASTC itself is
     * emulated on hosts without it (vk_texcomp.cpp); HDR ASTC is not, so it stays unlisted.
     */
    private static final String EXTENSIONS = String.join(" ",
            "GL_OES_EGL_image", "GL_OES_EGL_image_external", "GL_OES_EGL_sync", "GL_OES_vertex_half_float",
            "GL_OES_framebuffer_object", "GL_OES_rgb8_rgba8", "GL_OES_compressed_ETC1_RGB8_texture",
            "GL_AMD_compressed_ATC_texture", "GL_KHR_texture_compression_astc_ldr",
            "GL_OES_texture_compression_astc", "GL_OES_texture_npot", "GL_EXT_texture_filter_anisotropic",
            "GL_EXT_texture_format_BGRA8888", "GL_EXT_read_format_bgra", "GL_OES_texture_3D",
            "GL_EXT_color_buffer_float", "GL_EXT_color_buffer_half_float", "GL_EXT_float_blend",
            "GL_QCOM_alpha_test", "GL_OES_depth24", "GL_OES_packed_depth_stencil", "GL_OES_depth_texture",
            "GL_OES_depth_texture_cube_map", "GL_EXT_sRGB", "GL_OES_texture_float", "GL_OES_texture_float_linear",
            "GL_OES_texture_half_float", "GL_OES_texture_half_float_linear", "GL_EXT_texture_type_2_10_10_10_REV",
            "GL_EXT_texture_sRGB_decode", "GL_EXT_texture_format_sRGB_override", "GL_OES_element_index_uint",
            "GL_EXT_copy_image", "GL_EXT_geometry_shader", "GL_EXT_tessellation_shader", "GL_OES_texture_stencil8",
            "GL_EXT_shader_io_blocks", "GL_OES_shader_image_atomic", "GL_OES_sample_variables",
            "GL_EXT_texture_border_clamp", "GL_EXT_EGL_image_external_wrap_modes", "GL_EXT_multisampled_render_to_texture",
            "GL_EXT_multisampled_render_to_texture2", "GL_OES_shader_multisample_interpolation",
            "GL_EXT_texture_cube_map_array", "GL_EXT_draw_buffers_indexed", "GL_EXT_gpu_shader5",
            "GL_EXT_robustness", "GL_EXT_texture_buffer", "GL_EXT_shader_framebuffer_fetch",
            "GL_ARM_shader_framebuffer_fetch_depth_stencil", "GL_OES_texture_storage_multisample_2d_array",
            "GL_OES_sample_shading", "GL_OES_get_program_binary", "GL_EXT_debug_label", "GL_KHR_blend_equation_advanced",
            "GL_KHR_blend_equation_advanced_coherent", "GL_QCOM_tiled_rendering", "GL_ANDROID_extension_pack_es31a",
            "GL_EXT_primitive_bounding_box", "GL_OES_standard_derivatives", "GL_OES_vertex_array_object",
            "GL_EXT_disjoint_timer_query", "GL_KHR_debug", "GL_EXT_YUV_target", "GL_EXT_sRGB_write_control",
            "GL_KHR_no_error", "GL_EXT_texture_sRGB_R8", "GL_EXT_texture_sRGB_RG8", "GL_EXT_buffer_storage",
            "GL_EXT_external_buffer", "GL_EXT_blit_framebuffer_params", "GL_EXT_clip_cull_distance",
            "GL_EXT_protected_textures", "GL_EXT_shader_non_constant_global_initializers", "GL_QCOM_texture_foveated",
            "GL_QCOM_shader_framebuffer_fetch_noncoherent", "GL_OVR_multiview", "GL_OVR_multiview2",
            "GL_OVR_multiview_multisampled_render_to_texture", "GL_EXT_fragment_invocation_density",
            "GL_KHR_robustness", "GL_KHR_robust_buffer_access_behavior", "GL_EXT_memory_object",
            "GL_EXT_memory_object_fd", "GL_EXT_semaphore", "GL_EXT_semaphore_fd", "GL_EXT_EGL_image_storage",
            "GL_EXT_EGL_image_array", "GL_EXT_texture_shadow_lod");

    public GLES20() {}

    public static String glGetString(int name) {
        switch (name) {
            case GL_VENDOR: return "Qualcomm";
            case GL_RENDERER: return "Adreno (TM) 650";
            case GL_VERSION: return "OpenGL ES 3.2 V@0502.0 (GIT@d4cfdf3, Ic907de5ed0, 1601055299) (Date:09/25/20)";
            case GL_SHADING_LANGUAGE_VERSION: return "OpenGL ES GLSL ES 3.20";
            case GL_EXTENSIONS: return EXTENSIONS;
            default: return null;
        }
    }

    public static int glGetError() {
        return 0;
    }
}
