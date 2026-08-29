// TextBlockStream.cpp: Implementation of class `TextBlockStream`.

#include "ipasim/TextBlockStream.hpp"

#include <iostream>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.Documents.h>
#include <winrt/Windows.UI.Xaml.Media.h>

using namespace ipasim;
using namespace winrt;
using namespace Windows::UI;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Documents;
using namespace Windows::UI::Xaml::Media;

void TextBlockStream::write(const hstring &S) {
  TextBlock TB(TBP.get());

  // Console tester hosts intentionally have no XAML TextBlock. Keep the same
  // log stream useful there instead of dereferencing a null UI object; the
  // normal application continues to use its TextBlock when one is initialized.
  if (!TB) {
    const std::string Narrow = to_string(S);
    if (Error)
      std::cerr << Narrow;
    else
      std::cout << Narrow;
    return;
  }

  TB.Dispatcher().RunAsync(CoreDispatcherPriority::Normal, [this, S, TB]() {
    Run R;
    R.Text(S);
    if (Error)
      R.Foreground(SolidColorBrush(Colors::Red()));
    TB.Inlines().Append(R);
  });
}
