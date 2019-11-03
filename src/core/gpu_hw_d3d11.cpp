#include "gpu_hw_d3d11.h"
#include "YBaseLib/Assert.h"
#include "YBaseLib/Log.h"
#include "YBaseLib/String.h"
#include "common/d3d11/shader_compiler.h"
#include "gpu_hw_shadergen.h"
#include "host_display.h"
#include "host_interface.h"
#include "imgui.h"
#include "system.h"
Log_SetChannel(GPU_HW_D3D11);

GPU_HW_D3D11::GPU_HW_D3D11() = default;

GPU_HW_D3D11::~GPU_HW_D3D11()
{
  m_host_display->SetDisplayTexture(nullptr, 0, 0, 0, 0, 0, 0, 1.0f);
}

bool GPU_HW_D3D11::Initialize(HostDisplay* host_display, System* system, DMA* dma,
                              InterruptController* interrupt_controller, Timers* timers)
{
  SetCapabilities();

  if (!GPU_HW::Initialize(host_display, system, dma, interrupt_controller, timers))
    return false;

  if (host_display->GetRenderAPI() != HostDisplay::RenderAPI::D3D11)
  {
    Log_ErrorPrintf("Host render API is incompatible");
    return false;
  }

  m_device = static_cast<ID3D11Device*>(host_display->GetHostRenderDevice());
  m_context = static_cast<ID3D11DeviceContext*>(host_display->GetHostRenderContext());
  if (!m_device || !m_context)
    return false;

  CreateFramebuffer();
  CreateVertexBuffer();
  CreateUniformBuffer();
  CreateTextureBuffer();
  if (!CompileShaders() || !CreateBatchInputLayout())
    return false;

  RestoreGraphicsAPIState();
  return true;
}

void GPU_HW_D3D11::Reset()
{
  GPU_HW::Reset();

  ClearFramebuffer();
}

void GPU_HW_D3D11::ResetGraphicsAPIState()
{
  GPU_HW::ResetGraphicsAPIState();

#if 0
  glEnable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDepthMask(GL_TRUE);
  glLineWidth(1.0f);
  glBindVertexArray(0);
#endif
}

void GPU_HW_D3D11::RestoreGraphicsAPIState()
{
#if 0
  m_vram_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
  glViewport(0, 0, m_vram_texture->GetWidth(), m_vram_texture->GetHeight());

  glDisable(GL_CULL_FACE);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_SCISSOR_TEST);
  glDepthMask(GL_FALSE);
  glLineWidth(static_cast<float>(m_resolution_scale));
  UpdateDrawingArea();

  glBindVertexArray(m_vao_id);
#endif
}

void GPU_HW_D3D11::UpdateSettings()
{
  GPU_HW::UpdateSettings();

  CreateFramebuffer();
  CompileShaders();
  UpdateDisplay();
}

void GPU_HW_D3D11::DrawRendererStatsWindow()
{
  GPU_HW::DrawRendererStatsWindow();

  ImGui::SetNextWindowSize(ImVec2(300.0f, 150.0f), ImGuiCond_FirstUseEver);

  const bool is_null_frame = m_stats.num_batches == 0;
  if (!is_null_frame)
  {
    m_last_stats = m_stats;
    m_stats = {};
  }

  if (ImGui::Begin("GPU Renderer Statistics", &m_show_renderer_statistics))
  {
    ImGui::Columns(2);
    ImGui::SetColumnWidth(0, 200.0f);

    ImGui::TextUnformatted("GPU Active In This Frame: ");
    ImGui::NextColumn();
    ImGui::Text("%s", is_null_frame ? "Yes" : "No");
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Reads: ");
    ImGui::NextColumn();
    ImGui::Text("%u", m_last_stats.num_vram_reads);
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Writes: ");
    ImGui::NextColumn();
    ImGui::Text("%u", m_last_stats.num_vram_writes);
    ImGui::NextColumn();

    ImGui::TextUnformatted("VRAM Read Texture Updates:");
    ImGui::NextColumn();
    ImGui::Text("%u", m_last_stats.num_vram_read_texture_updates);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Batches Drawn:");
    ImGui::NextColumn();
    ImGui::Text("%u", m_last_stats.num_batches);
    ImGui::NextColumn();

    ImGui::TextUnformatted("Vertices Drawn: ");
    ImGui::NextColumn();
    ImGui::Text("%u", m_last_stats.num_vertices);
    ImGui::NextColumn();
  }

  ImGui::End();
}

void GPU_HW_D3D11::InvalidateVRAMReadCache()
{
  m_vram_read_texture_dirty = true;
}

void GPU_HW_D3D11::MapBatchVertexPointer(u32 required_vertices)
{
  Assert(!m_batch_start_vertex_ptr);

  const D3D11::StreamBuffer::MappingResult res =
    m_vertex_stream_buffer.Map(m_context.Get(), sizeof(BatchVertex), required_vertices * sizeof(BatchVertex));

  m_batch_start_vertex_ptr = static_cast<BatchVertex*>(res.pointer);
  m_batch_current_vertex_ptr = m_batch_start_vertex_ptr;
  m_batch_end_vertex_ptr = m_batch_start_vertex_ptr + res.space_aligned;
  m_batch_base_vertex = res.index_aligned;
}

void GPU_HW_D3D11::SetCapabilities()
{
  m_max_resolution_scale = 1;
  Log_InfoPrintf("Maximum resolution scale is %u", m_max_resolution_scale);
}

bool GPU_HW_D3D11::CreateFramebuffer()
{
  // save old vram texture/fbo, in case we're changing scale
  auto old_vram_texture = std::move(m_vram_texture);
  DestroyFramebuffer();

  // scale vram size to internal resolution
  const u32 texture_width = VRAM_WIDTH * m_resolution_scale;
  const u32 texture_height = VRAM_HEIGHT * m_resolution_scale;
  const DXGI_FORMAT texture_format = DXGI_FORMAT_R8G8B8A8_UNORM;

  if (!m_vram_texture.Create(m_device.Get(), texture_width, texture_height, texture_format, true, true) ||
      !m_vram_read_texture.Create(m_device.Get(), texture_width, texture_height, texture_format, true, false) ||
      !m_display_texture.Create(m_device.Get(), texture_width, texture_height, texture_format, true, true))
  {
    return false;
  }

  // do we need to restore the framebuffer after a size change?
  if (old_vram_texture)
  {
    const bool linear_filter = old_vram_texture.GetWidth() > m_vram_texture.GetWidth();
    Log_DevPrintf("Scaling %ux%u VRAM texture to %ux%u using %s filter", old_vram_texture.GetWidth(),
                  old_vram_texture.GetHeight(), m_vram_texture.GetWidth(), m_vram_texture.GetHeight(),
                  linear_filter ? "linear" : "nearest");

#if 0
    glDisable(GL_SCISSOR_TEST);
    old_vram_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
    glBlitFramebuffer(0, 0, old_vram_texture->GetWidth(), old_vram_texture->GetHeight(), 0, 0,
                      m_vram_texture->GetWidth(), m_vram_texture->GetHeight(), GL_COLOR_BUFFER_BIT,
                      linear_filter ? GL_LINEAR : GL_NEAREST);

    glEnable(GL_SCISSOR_TEST);
#endif
  }

  if (m_resolution_scale > 1 &&
      !m_vram_downsample_texture.Create(m_device.Get(), texture_width, texture_height, texture_format, true, true))
  {
    return false;
  }

  m_context->OMSetRenderTargets(1, m_vram_texture.GetD3DRTVArray(), nullptr);
  m_vram_read_texture_dirty = true;
  return true;
}

void GPU_HW_D3D11::ClearFramebuffer()
{
  static constexpr std::array<float, 4> color = {};
  m_context->ClearRenderTargetView(m_vram_texture.GetD3DRTV(), color.data());
  m_vram_read_texture_dirty = true;
}

void GPU_HW_D3D11::DestroyFramebuffer()
{
  m_vram_read_texture.Destroy();
  m_vram_texture.Destroy();
  m_vram_downsample_texture.Destroy();
  m_display_texture.Destroy();
}

bool GPU_HW_D3D11::CreateVertexBuffer()
{
  return m_vertex_stream_buffer.Create(m_device.Get(), D3D11_BIND_VERTEX_BUFFER, VERTEX_BUFFER_SIZE);
}

bool GPU_HW_D3D11::CreateUniformBuffer()
{
  return m_uniform_stream_buffer.Create(m_device.Get(), D3D11_BIND_CONSTANT_BUFFER, UNIFORM_BUFFER_SIZE);
}

bool GPU_HW_D3D11::CreateTextureBuffer()
{
  if (!m_texture_stream_buffer.Create(m_device.Get(), D3D11_BIND_SHADER_RESOURCE, VRAM_UPDATE_TEXTURE_BUFFER_SIZE))
    return false;

  const CD3D11_SHADER_RESOURCE_VIEW_DESC srv_desc(D3D11_SRV_DIMENSION_BUFFER, DXGI_FORMAT_R16_UINT, 0,
                                                  VRAM_UPDATE_TEXTURE_BUFFER_SIZE / sizeof(u16));
  const HRESULT hr = m_device->CreateShaderResourceView(m_texture_stream_buffer.GetD3DBuffer(), &srv_desc,
                                                        m_texture_stream_buffer_srv_r16ui.ReleaseAndGetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Creation of texture buffer SRV failed: 0x%08X", hr);
    return false;
  }

  return true;
}

bool GPU_HW_D3D11::CreateBatchInputLayout()
{
  static constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 4> attributes = {
    {{"ATTR", 0, DXGI_FORMAT_R32G32_SINT, 0, offsetof(BatchVertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0},
     {"ATTR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(BatchVertex, color), D3D11_INPUT_PER_VERTEX_DATA, 0},
     {"ATTR", 2, DXGI_FORMAT_R32_SINT, 0, offsetof(BatchVertex, texcoord), D3D11_INPUT_PER_VERTEX_DATA, 0},
     {"ATTR", 3, DXGI_FORMAT_R32_SINT, 0, offsetof(BatchVertex, texpage), D3D11_INPUT_PER_VERTEX_DATA, 0}}};

  // we need a vertex shader...
  GPU_HW_ShaderGen shadergen(GPU_HW_ShaderGen::API::D3D11, m_resolution_scale, m_true_color);
  ComPtr<ID3DBlob> vs_bytecode = D3D11::ShaderCompiler::CompileShader(
    D3D11::ShaderCompiler::Type::Vertex, m_device->GetFeatureLevel(), shadergen.GenerateBatchVertexShader(true), false);
  if (!vs_bytecode)
    return false;

  const HRESULT hr = m_device->CreateInputLayout(attributes.data(), static_cast<UINT>(attributes.size()),
                                                 vs_bytecode->GetBufferPointer(), vs_bytecode->GetBufferSize(),
                                                 m_batch_input_layout.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("CreateInputLayout failed: 0x%08X", hr);
    return false;
  }

  return true;
}

bool GPU_HW_D3D11::CreateStateObjects()
{
  HRESULT hr;

  CD3D11_RASTERIZER_DESC rs_desc = CD3D11_RASTERIZER_DESC(CD3D11_DEFAULT());
  rs_desc.CullMode = D3D11_CULL_NONE;
  rs_desc.ScissorEnable = TRUE;
  hr = m_device->CreateRasterizerState(&rs_desc, m_cull_none_rasterizer_state.GetAddressOf());
  if (FAILED(hr))
    return false;

  CD3D11_DEPTH_STENCIL_DESC ds_desc = CD3D11_DEPTH_STENCIL_DESC(CD3D11_DEFAULT());
  ds_desc.DepthEnable = FALSE;
  ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  hr = m_device->CreateDepthStencilState(&ds_desc, m_depth_disabled_state.GetAddressOf());
  if (FAILED(hr))
    return false;

  CD3D11_BLEND_DESC bl_desc = CD3D11_BLEND_DESC(CD3D11_DEFAULT());
  hr = m_device->CreateBlendState(&bl_desc, m_blend_disabled_state.GetAddressOf());
  if (FAILED(hr))
    return false;

  m_batch_blend_states[static_cast<u8>(TransparencyMode::Disabled)] = m_blend_disabled_state;

  for (u8 transparency_mode = 0; transparency_mode < 4; transparency_mode++)
  {
    bl_desc.RenderTarget[0].BlendEnable = TRUE;
    bl_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl_desc.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_ALPHA;
    bl_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bl_desc.RenderTarget[0].BlendOp =
      (transparency_mode == static_cast<u8>(TransparencyMode::BackgroundMinusForeground)) ?
        D3D11_BLEND_OP_REV_SUBTRACT :
        D3D11_BLEND_OP_ADD;
    bl_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;

    hr = m_device->CreateBlendState(&bl_desc, m_batch_blend_states[transparency_mode].GetAddressOf());
    if (FAILED(hr))
      return false;
  }

  return true;
}

bool GPU_HW_D3D11::CompileShaders()
{
  GPU_HW_ShaderGen shadergen(GPU_HW_ShaderGen::API::D3D11, m_resolution_scale, m_true_color);
  const bool debug = true;

  m_screen_quad_vertex_shader = D3D11::ShaderCompiler::CompileAndCreateVertexShader(
    m_device.Get(), shadergen.GenerateScreenQuadVertexShader(), debug);
  if (!m_screen_quad_vertex_shader)
    return false;

  for (u8 textured = 0; textured < 2; textured++)
  {
    const std::string vs = shadergen.GenerateBatchVertexShader(ConvertToBoolUnchecked(textured));
    m_batch_vertex_shaders[textured] = D3D11::ShaderCompiler::CompileAndCreateVertexShader(m_device.Get(), vs, debug);
    if (!m_batch_vertex_shaders[textured])
      return false;
  }

  for (u8 render_mode = 0; render_mode < 4; render_mode++)
  {
    for (u8 texture_mode = 0; texture_mode < 9; texture_mode++)
    {
      for (u8 dithering = 0; dithering < 2; dithering++)
      {
        const std::string ps = shadergen.GenerateBatchFragmentShader(static_cast<BatchRenderMode>(render_mode),
                                                                     static_cast<TextureMode>(texture_mode),
                                                                     ConvertToBoolUnchecked(dithering));

        m_batch_pixel_shaders[render_mode][texture_mode][dithering] =
          D3D11::ShaderCompiler::CompileAndCreatePixelShader(m_device.Get(), ps, debug);
        if (!m_batch_pixel_shaders[render_mode][texture_mode][dithering])
          return false;
      }
    }
  }

  m_copy_pixel_shader =
    D3D11::ShaderCompiler::CompileAndCreatePixelShader(m_device.Get(), shadergen.GenerateCopyFragmentShader(), debug);
  if (!m_copy_pixel_shader)
    return false;

  m_fill_pixel_shader =
    D3D11::ShaderCompiler::CompileAndCreatePixelShader(m_device.Get(), shadergen.GenerateFillFragmentShader(), debug);
  if (!m_fill_pixel_shader)
    return false;

  m_vram_write_pixel_shader = D3D11::ShaderCompiler::CompileAndCreatePixelShader(
    m_device.Get(), shadergen.GenerateVRAMWriteFragmentShader(), debug);
  if (!m_vram_write_pixel_shader)
    return false;

  for (u8 depth_24bit = 0; depth_24bit < 2; depth_24bit++)
  {
    for (u8 interlaced = 0; interlaced < 2; interlaced++)
    {
      const std::string ps = shadergen.GenerateDisplayFragmentShader(ConvertToBoolUnchecked(depth_24bit),
                                                                     ConvertToBoolUnchecked(interlaced));
      m_display_pixel_shaders[depth_24bit][interlaced] =
        D3D11::ShaderCompiler::CompileAndCreatePixelShader(m_device.Get(), ps, debug);
      if (!m_display_pixel_shaders[depth_24bit][interlaced])
        return false;
    }
  }

  return true;
}

void GPU_HW_D3D11::SetDrawState(BatchRenderMode render_mode)
{
  const bool textured = (m_batch.texture_mode != TextureMode::Disabled);

  static constexpr std::array<D3D11_PRIMITIVE_TOPOLOGY, 4> d3d_primitives = {
    {D3D11_PRIMITIVE_TOPOLOGY_LINELIST, D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
     D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP}};
  const UINT stride = sizeof(BatchVertex);
  const UINT offset = 0;
  m_context->IASetPrimitiveTopology(d3d_primitives[static_cast<u8>(m_batch.primitive)]);
  m_context->IASetInputLayout(m_batch_input_layout.Get());
  m_context->IASetVertexBuffers(0, 1, m_vertex_stream_buffer.GetD3DBufferArray(), &stride, &offset);

  m_context->VSSetShader(m_batch_vertex_shaders[BoolToUInt8(textured)].Get(), nullptr, 0);

  m_context->PSSetShader(m_batch_pixel_shaders[static_cast<u8>(render_mode)][static_cast<u8>(m_batch.texture_mode)]
                                              [BoolToUInt8(m_batch.dithering)]
                                                .Get(),
                         nullptr, 0);
  m_context->PSSetShaderResources(0, 1, m_vram_read_texture.GetD3DSRVArray());

  const TransparencyMode transparency_mode =
    (render_mode == BatchRenderMode::OnlyOpaque) ? TransparencyMode::Disabled : m_batch.transparency_mode;
  m_context->OMSetBlendState(m_batch_blend_states[static_cast<u8>(transparency_mode)].Get(), nullptr, 0xFFFFFFFFu);

  // if (m_drawing_area_changed)
  {
    m_drawing_area_changed = false;

    int left, top, right, bottom;
    CalcScissorRect(&left, &top, &right, &bottom);

    CD3D11_RECT rc(left, top, right, bottom);
    m_context->RSSetScissorRects(1, &rc);
  }

  const CD3D11_VIEWPORT vp(0.0f, 0.0f, static_cast<float>(m_vram_texture.GetWidth()),
                           static_cast<float>(m_vram_texture.GetHeight()));
  m_context->RSSetViewports(1, &vp);

  if (m_batch_ubo_dirty)
  {
    UploadUniformBlock(&m_batch_ubo_data, sizeof(m_batch_ubo_data));
    m_batch_ubo_dirty = false;
  }
}

void GPU_HW_D3D11::UploadUniformBlock(const void* data, u32 data_size)
{
  const auto res = m_uniform_stream_buffer.Map(m_context.Get(), m_uniform_buffer_alignment, data_size);
  std::memcpy(res.pointer, data, data_size);
  m_uniform_stream_buffer.Unmap(m_context.Get(), data_size);

  m_context->VSSetConstantBuffers(0, 1, m_uniform_stream_buffer.GetD3DBufferArray());
  m_context->PSSetConstantBuffers(0, 1, m_uniform_stream_buffer.GetD3DBufferArray());

  m_stats.num_uniform_buffer_updates++;
}

void GPU_HW_D3D11::UpdateDrawingArea()
{
  m_drawing_area_changed = true;
}

void GPU_HW_D3D11::UpdateDisplay()
{
  GPU_HW::UpdateDisplay();

  if (m_system->GetSettings().debugging.show_vram)
  {
    m_host_display->SetDisplayTexture(m_vram_texture.GetD3DSRV(), 0, 0, m_vram_texture.GetWidth(),
                                      m_vram_texture.GetHeight(), m_vram_texture.GetWidth(), m_vram_texture.GetHeight(),
                                      1.0f);
  }
  else
  {
#if 0
    const u32 vram_offset_x = m_crtc_state.regs.X;
    const u32 vram_offset_y = m_crtc_state.regs.Y;
    const u32 scaled_vram_offset_x = vram_offset_x * m_resolution_scale;
    const u32 scaled_vram_offset_y = vram_offset_y * m_resolution_scale;
    const u32 display_width = std::min<u32>(m_crtc_state.display_width, VRAM_WIDTH - vram_offset_x);
    const u32 display_height = std::min<u32>(m_crtc_state.display_height << BoolToUInt8(m_GPUSTAT.vertical_interlace),
                                             VRAM_HEIGHT - vram_offset_y);
    const u32 scaled_display_width = display_width * m_resolution_scale;
    const u32 scaled_display_height = display_height * m_resolution_scale;
    const u32 flipped_vram_offset_y = VRAM_HEIGHT - vram_offset_y - display_height;
    const u32 scaled_flipped_vram_offset_y = m_vram_texture->GetHeight() - scaled_vram_offset_y - scaled_display_height;

    if (m_GPUSTAT.display_disable)
    {
      m_system->GetHostInterface()->SetDisplayTexture(nullptr, 0, 0, 0, 0, m_crtc_state.display_aspect_ratio);
    }
    else if (!m_GPUSTAT.display_area_color_depth_24 && !m_GPUSTAT.vertical_interlace)
    {
      // fast path when both interlacing and 24-bit depth is off
      glCopyImageSubData(m_vram_texture->GetGLId(), GL_TEXTURE_2D, 0, scaled_vram_offset_x,
                         scaled_flipped_vram_offset_y, 0, m_display_texture->GetGLId(), GL_TEXTURE_2D, 0, 0, 0, 0,
                         scaled_display_width, scaled_display_height, 1);

      m_system->GetHostInterface()->SetDisplayTexture(m_display_texture.get(), 0, 0, scaled_display_width,
                                                      scaled_display_height, m_crtc_state.display_aspect_ratio);
    }
    else
    {
      const u32 field_offset = BoolToUInt8(m_GPUSTAT.vertical_interlace && !m_GPUSTAT.drawing_even_line);
      const u32 scaled_field_offset = field_offset * m_resolution_scale;

      glDisable(GL_BLEND);
      glDisable(GL_SCISSOR_TEST);

      const GL::Program& prog = m_display_programs[BoolToUInt8(m_GPUSTAT.display_area_color_depth_24)]
                                                  [BoolToUInt8(m_GPUSTAT.vertical_interlace)];
      prog.Bind();

      // Because of how the reinterpret shader works, we need to use the downscaled version.
      if (m_GPUSTAT.display_area_color_depth_24 && m_resolution_scale > 1)
      {
        const u32 copy_width = std::min<u32>((display_width * 4) / 3, VRAM_WIDTH - vram_offset_x);
        const u32 scaled_copy_width = copy_width * m_resolution_scale;
        m_vram_downsample_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
        m_vram_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
        glBlitFramebuffer(scaled_vram_offset_x, scaled_flipped_vram_offset_y, scaled_vram_offset_x + scaled_copy_width,
                          scaled_flipped_vram_offset_y + scaled_display_height, vram_offset_x, flipped_vram_offset_y,
                          vram_offset_x + copy_width, flipped_vram_offset_y + display_height, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);

        m_display_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
        m_vram_downsample_texture->Bind();

        glViewport(0, field_offset, display_width, display_height);

        const u32 uniforms[4] = {vram_offset_x, flipped_vram_offset_y, field_offset};
        UploadUniformBlock(uniforms, sizeof(uniforms));
        m_batch_ubo_dirty = true;

        glDrawArrays(GL_TRIANGLES, 0, 3);

        m_system->GetHostInterface()->SetDisplayTexture(m_display_texture.get(), 0, 0, display_width, display_height,
                                                        m_crtc_state.display_aspect_ratio);
      }
      else
      {
        m_display_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
        m_vram_texture->Bind();

        glViewport(0, scaled_field_offset, scaled_display_width, scaled_display_height);

        const u32 uniforms[4] = {scaled_vram_offset_x, scaled_flipped_vram_offset_y, scaled_field_offset};
        UploadUniformBlock(uniforms, sizeof(uniforms));
        m_batch_ubo_dirty = true;

        glDrawArrays(GL_TRIANGLES, 0, 3);

        m_system->GetHostInterface()->SetDisplayTexture(m_display_texture.get(), 0, 0, scaled_display_width,
                                                        scaled_display_height, m_crtc_state.display_aspect_ratio);
      }

      // restore state
      m_vram_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
      glViewport(0, 0, m_vram_texture->GetWidth(), m_vram_texture->GetHeight());
      glEnable(GL_SCISSOR_TEST);
    }
#endif
  }
}

void GPU_HW_D3D11::ReadVRAM(u32 x, u32 y, u32 width, u32 height, void* buffer)
{
#if 0
  // we need to convert RGBA8 -> RGBA5551
  std::vector<u32> temp_buffer(width * height);
  const u32 flipped_y = VRAM_HEIGHT - y - height;

  // downscaling to 1xIR.
  if (m_resolution_scale > 1)
  {
    const u32 texture_height = m_vram_texture->GetHeight();
    const u32 scaled_x = x * m_resolution_scale;
    const u32 scaled_y = y * m_resolution_scale;
    const u32 scaled_width = width * m_resolution_scale;
    const u32 scaled_height = height * m_resolution_scale;
    const u32 scaled_flipped_y = texture_height - scaled_y - scaled_height;

    m_vram_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
    m_vram_downsample_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(scaled_x, scaled_flipped_y, scaled_x + scaled_width, scaled_flipped_y + scaled_height, 0, 0,
                      width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glEnable(GL_SCISSOR_TEST);
    m_vram_texture->BindFramebuffer(GL_DRAW_FRAMEBUFFER);
    m_vram_downsample_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, temp_buffer.data());
  }
  else
  {
    m_vram_texture->BindFramebuffer(GL_READ_FRAMEBUFFER);
    glReadPixels(x, flipped_y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, temp_buffer.data());
  }

  // reverse copy because of lower-left origin
  const u32 source_stride = width * sizeof(u32);
  const u8* source_ptr = reinterpret_cast<const u8*>(temp_buffer.data()) + (source_stride * (height - 1));
  const u32 dst_stride = width * sizeof(u16);
  u8* dst_ptr = static_cast<u8*>(buffer);
  for (u32 row = 0; row < height; row++)
  {
    const u8* source_row_ptr = source_ptr;
    u8* dst_row_ptr = dst_ptr;

    for (u32 col = 0; col < width; col++)
    {
      u32 src_col;
      std::memcpy(&src_col, source_row_ptr, sizeof(src_col));
      source_row_ptr += sizeof(src_col);

      const u16 dst_col = RGBA8888ToRGBA5551(src_col);
      std::memcpy(dst_row_ptr, &dst_col, sizeof(dst_col));
      dst_row_ptr += sizeof(dst_col);
    }

    source_ptr -= source_stride;
    dst_ptr += dst_stride;
  }

#endif
  m_stats.num_vram_reads++;
}

void GPU_HW_D3D11::FillVRAM(u32 x, u32 y, u32 width, u32 height, u32 color)
{
  // scale coordinates
  x *= m_resolution_scale;
  y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  // drop precision unless true colour is enabled
  if (!m_true_color)
    color = RGBA5551ToRGBA8888(RGBA8888ToRGBA5551(color));

  m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_context->VSSetShader(m_screen_quad_vertex_shader.Get(), nullptr, 0);
  m_context->PSSetShader(m_fill_pixel_shader.Get(), nullptr, 0);

  float uniform[4];
  std::tie(uniform[0], uniform[1], uniform[2], uniform[3]) = RGBA8ToFloat(color);
  UploadUniformBlock(uniform, sizeof(uniform));
  m_batch_ubo_dirty = true;

  CD3D11_RECT scissor_rc(x, y, x + width, y + height);
  m_context->RSSetScissorRects(1, &scissor_rc);
  m_context->OMSetBlendState(m_blend_disabled_state.Get(), nullptr, 0xFFFFFFFFu);

  m_context->Draw(3, 0);

  UpdateDrawingArea();
  InvalidateVRAMReadCache();
}

void GPU_HW_D3D11::UpdateVRAM(u32 x, u32 y, u32 width, u32 height, const void* data)
{
  const u32 num_pixels = width * height;
  const auto map_result = m_texture_stream_buffer.Map(m_context.Get(), sizeof(u16), num_pixels * sizeof(u16));
  std::memcpy(map_result.pointer, data, num_pixels * sizeof(u16));
  m_texture_stream_buffer.Unmap(m_context.Get(), num_pixels * sizeof(u16));

  m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_context->VSSetShader(m_screen_quad_vertex_shader.Get(), nullptr, 0);
  m_context->PSSetShader(m_vram_write_pixel_shader.Get(), nullptr, 0);
  m_context->PSSetShaderResources(0, 1, m_texture_stream_buffer_srv_r16ui.GetAddressOf());

  const u32 uniforms[5] = {x, y, width, height, map_result.index_aligned};
  UploadUniformBlock(uniforms, sizeof(uniforms));
  m_batch_ubo_dirty = true;

  // viewport should be set to the whole VRAM size, so we can just set the scissor
  const CD3D11_RECT scissor_rc(x * m_resolution_scale, y * m_resolution_scale, x * m_resolution_scale + width,
                               y * m_resolution_scale + height);
  m_context->RSSetScissorRects(1, &scissor_rc);
  m_context->OMSetBlendState(m_blend_disabled_state.Get(), nullptr, 0xFFFFFFFFu);

  m_context->Draw(3, 0);

  UpdateDrawingArea();
  InvalidateVRAMReadCache();
  m_stats.num_vram_writes++;
}

void GPU_HW_D3D11::CopyVRAM(u32 src_x, u32 src_y, u32 dst_x, u32 dst_y, u32 width, u32 height)
{
  src_x *= m_resolution_scale;
  src_y *= m_resolution_scale;
  dst_x *= m_resolution_scale;
  dst_y *= m_resolution_scale;
  width *= m_resolution_scale;
  height *= m_resolution_scale;

  const CD3D11_BOX src_box(src_x, src_y, 0, src_x + width, src_y + height, 1);
  m_context->CopySubresourceRegion(m_vram_texture, 0, dst_x, dst_y, 0, m_vram_texture, 0, &src_box);
  InvalidateVRAMReadCache();
}

void GPU_HW_D3D11::UpdateVRAMReadTexture()
{
  m_stats.num_vram_read_texture_updates++;
  m_vram_read_texture_dirty = false;

  const CD3D11_BOX src_box(0, 0, 0, m_vram_texture.GetWidth(), m_vram_texture.GetHeight(), 1);
  m_context->CopySubresourceRegion(m_vram_read_texture, 0, 0, 0, 0, m_vram_texture, 0, &src_box);
}

void GPU_HW_D3D11::FlushRender()
{
  const u32 vertex_count = GetBatchVertexCount();
  if (vertex_count == 0)
    return;

  if (m_vram_read_texture_dirty)
    UpdateVRAMReadTexture();

  m_stats.num_batches++;
  m_stats.num_vertices += vertex_count;

  m_vertex_stream_buffer.Unmap(m_context.Get(), vertex_count * sizeof(BatchVertex));
  m_batch_start_vertex_ptr = nullptr;
  m_batch_end_vertex_ptr = nullptr;
  m_batch_current_vertex_ptr = nullptr;

  if (m_batch.NeedsTwoPassRendering())
  {
    SetDrawState(BatchRenderMode::OnlyTransparent);
    m_context->Draw(vertex_count, m_batch_base_vertex);
    SetDrawState(BatchRenderMode::OnlyOpaque);
    m_context->Draw(vertex_count, m_batch_base_vertex);
  }
  else
  {
    SetDrawState(m_batch.GetRenderMode());
    m_context->Draw(vertex_count, m_batch_base_vertex);
  }
}

std::unique_ptr<GPU> GPU::CreateHardwareD3D11Renderer()
{
  return std::make_unique<GPU_HW_D3D11>();
}
