#pragma once
#include "../UIView.h"

namespace ed
{
	class DebugWatchUI : public UIView
	{
	public:
		using UIView::UIView;

		virtual void OnEvent(const SDL_Event& e);
		virtual void Update(float delta);
	};
}