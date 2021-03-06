//
// Copyright(c) 2017-2018 Pawe� Ksi�opolski ( pumexx )
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#pragma once
#include <vector>
#include <functional>
#include <mutex>

namespace pumex
{

// Handy class that may transfer actions between threads
class ActionQueue
{
public:
  explicit ActionQueue()                     = default;
  ActionQueue(const ActionQueue&)            = delete;
  ActionQueue& operator=(const ActionQueue&) = delete;
  ActionQueue(ActionQueue&&)                 = delete;
  ActionQueue& operator=(ActionQueue&&)      = delete;

  void addAction(const std::function<void(void)>& fun)
  {
    std::lock_guard<std::mutex> lock(mutex);
    actions.push_back(fun);
  }
  void performActions()
  {
    std::vector<std::function<void(void)>> actionCopy;
    {
      std::lock_guard<std::mutex> lock(mutex);
      actionCopy = actions;
      actions.resize(0);
    }
    for (auto a : actionCopy)
      a();
  }
private:
  std::vector<std::function<void(void)>> actions;
  mutable std::mutex mutex;
};

}
