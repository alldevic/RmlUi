#include "RmlUi_Backend.h"
#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_SDL_GPU.h"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Log.h>

// When true, the GPU device is created in debug mode: SDL then validates its own calls, loads the driver's validation
// layers where it has them, and names resources for graphics debuggers. Turn it on from the build with the CMake
// option -DRMLUI_BACKEND_SDL_GPU_DEBUG=ON, which defines the macro for every file of the backend. Defining it here
// alone would only reach this one: the renderer keys its debug groups and frame statistics off the same macro.
#ifndef RMLUI_BACKEND_SDL_GPU_DEBUG
	#define RMLUI_BACKEND_SDL_GPU_DEBUG false
#endif

/**
    Global data used by this backend.

    Lifetime governed by the calls to Backend::Initialize() and Backend::Shutdown().
 */
struct BackendData {
	BackendData(SDL_Window* window, SDL_GPUDevice* device) : system_interface(window), render_interface(device, window) {}

	SystemInterface_SDL system_interface;
	RenderInterface_SDL_GPU render_interface;
	TextInputMethodEditor_SDL text_input_method_editor;

	SDL_Window* window = nullptr;
	SDL_GPUDevice* device = nullptr;
	SDL_GPUCommandBuffer* command_buffer = nullptr;

	bool running = true;
};
static Rml::UniquePtr<BackendData> data;

bool Backend::Initialize(const char* window_name, int width, int height, bool allow_resize)
{
	RMLUI_ASSERT(!data);

	SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");

	if (!SDL_Init(SDL_INIT_VIDEO))
		return false;

	// Submit click events when focusing the window.
	SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
	// Touch events are handled natively, no need to generate synthetic mouse events for touch devices.
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

#if defined RMLUI_BACKEND_SIMULATE_TOUCH
	// Simulate touch events from mouse events for testing touch behavior on a desktop machine.
	SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
#endif

	const float window_size_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, window_name);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, int(width * window_size_scale));
	SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, int(height * window_size_scale));
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, allow_resize);
	SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
	SDL_Window* window = SDL_CreateWindowWithProperties(props);
	SDL_DestroyProperties(props);

	if (!window)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR, "SDL error on create window: %s", SDL_GetError());
		return false;
	}

	props = SDL_CreateProperties();
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, RMLUI_BACKEND_SDL_GPU_DEBUG);
	SDL_GPUDevice* device = SDL_CreateGPUDeviceWithProperties(props);
	SDL_DestroyProperties(props);

	if (!device)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR, "SDL error on create GPU device: %s", SDL_GetError());
		return false;
	}

	if (!SDL_ClaimWindowForGPUDevice(device, window))
	{
		Rml::Log::Message(Rml::Log::LT_ERROR, "SDL error on claiming window for GPU device: %s", SDL_GetError());
		return false;
	}

	data = Rml::MakeUnique<BackendData>(window, device);
	data->window = window;
	data->device = device;

	const char* renderer_name = SDL_GetGPUDeviceDriver(device);
	data->system_interface.LogMessage(Rml::Log::LT_INFO, Rml::CreateString("Using SDL device driver: %s", renderer_name));

	Rml::SetTextInputHandler(&data->text_input_method_editor);

	return true;
}

void Backend::Shutdown()
{
	RMLUI_ASSERT(data);

	data->render_interface.Shutdown();

	SDL_ReleaseWindowFromGPUDevice(data->device, data->window);
	SDL_DestroyGPUDevice(data->device);
	SDL_DestroyWindow(data->window);

	data.reset();

	SDL_Quit();
}

Rml::SystemInterface* Backend::GetSystemInterface()
{
	RMLUI_ASSERT(data);
	return &data->system_interface;
}

Rml::RenderInterface* Backend::GetRenderInterface()
{
	RMLUI_ASSERT(data);
	return &data->render_interface;
}

bool Backend::ProcessEvents(Rml::Context* context, KeyDownCallback key_down_callback, bool power_save)
{
	RMLUI_ASSERT(data && context);

	auto GetKey = [](const SDL_Event& event) { return event.key.key; };
	auto GetDisplayScale = []() { return SDL_GetWindowDisplayScale(data->window); };
	constexpr auto event_quit = SDL_EVENT_QUIT;
	constexpr auto event_key_down = SDL_EVENT_KEY_DOWN;
	constexpr auto event_text_editing = SDL_EVENT_TEXT_EDITING;
	bool has_event = false;

	bool result = data->running;
	data->running = true;

	SDL_Event ev;
	if (power_save)
		has_event = SDL_WaitEventTimeout(&ev, static_cast<int>(Rml::Math::Min(context->GetNextUpdateDelay(), 10.0) * 1000));
	else
		has_event = SDL_PollEvent(&ev);

	while (has_event)
	{
		bool propagate_event = true;
		switch (ev.type)
		{
		case event_quit:
		{
			propagate_event = false;
			result = false;
		}
		break;
		case event_key_down:
		{
			propagate_event = false;
			const Rml::Input::KeyIdentifier key = RmlSDL::ConvertKey(GetKey(ev));
			const int key_modifier = RmlSDL::GetKeyModifierState();
			const float native_dp_ratio = GetDisplayScale();

			// See if we have any global shortcuts that take priority over the context.
			if (key_down_callback && !key_down_callback(context, key, key_modifier, native_dp_ratio, true))
				break;
			// Otherwise, hand the event over to the context by calling the input handler as normal.
			if (!RmlSDL::InputEventHandler(context, data->window, ev))
				break;
			// The key was not consumed by the context either, try keyboard shortcuts of lower priority.
			if (key_down_callback && !key_down_callback(context, key, key_modifier, native_dp_ratio, false))
				break;
		}
		break;
		case event_text_editing:
		{
			propagate_event = false;
			data->text_input_method_editor.HandleEdit(ev.edit);
		}
		break;
		default: break;
		}

		if (propagate_event)
			RmlSDL::InputEventHandler(context, data->window, ev);

		has_event = SDL_PollEvent(&ev);
	}

	return result;
}

void Backend::RequestExit()
{
	RMLUI_ASSERT(data);
	data->running = false;
}

void Backend::BeginFrame()
{
	RMLUI_ASSERT(data);

	data->command_buffer = SDL_AcquireGPUCommandBuffer(data->device);
	if (!data->command_buffer)
	{
		Rml::Log::Message(Rml::Log::LT_ERROR, "Failed to acquire command buffer: %s", SDL_GetError());
		return;
	}

	// The swapchain texture is taken by the renderer at the end of the frame, not here: it belongs to whichever
	// command buffer takes it, and the renderer may send the frame in several of them. So the frame is laid out for
	// the window's size, and whether there is anything to present at all is answered here instead -- a minimized
	// window has nothing to draw into and would otherwise be rendered every frame for nothing.
	int width = 0;
	int height = 0;
	SDL_GetWindowSizeInPixels(data->window, &width, &height);
	if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(data->window) & SDL_WINDOW_MINIMIZED))
	{
		// Not an error. The buffer is cleared along with being cancelled, so that PresentFrame() does not go on to
		// submit one that no longer exists.
		SDL_CancelGPUCommandBuffer(data->command_buffer);
		data->command_buffer = nullptr;
		return;
	}

	// No need to clear the swapchain texture: the renderer draws to its own layers and overwrites the whole swapchain
	// with the result in EndFrame().
	data->render_interface.BeginFrame(data->command_buffer, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
}

void Backend::PresentFrame()
{
	RMLUI_ASSERT(data);

	// Not necessarily the buffer BeginFrame() acquired: a frame may be sent in several, and the ones before the last
	// have already gone. Null when the frame was skipped, in which case the buffer has already been cancelled.
	if (SDL_GPUCommandBuffer* command_buffer = data->render_interface.EndFrame())
		SDL_SubmitGPUCommandBuffer(command_buffer);
	data->command_buffer = nullptr;
}
