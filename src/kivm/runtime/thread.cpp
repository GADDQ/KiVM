//
// Created by kiva on 2018/3/25.
//

#include <kivm/method.h>
#include <kivm/runtime/thread.h>
#include <kivm/runtime/runtimeConfig.h>
#include <kivm/bytecode/interpreter.h>
#include <kivm/oop/primitiveOop.h>

namespace kivm {
    Thread::Thread(Method *method, const std::list<oop> &args)
        : _frames(RuntimeConfig::get().threadMaxStackSize),
          _method(method), _args(args),
          _java_thread_object(nullptr), _native_thread(nullptr),
          _pc(0), _state(ThreadState::RUNNING) {
    }

    void Thread::create(instanceOop java_thread) {
        this->_java_thread_object = java_thread;
        this->_native_thread = new std::thread([this] {
            if (this->shouldRecordInThreadTable()) {
                D("should_record_in_thread_table == true, recording.");
                Threads::add(this);
            }
            this->start();
        });
        this->onThreadLaunched();
    }

    void Thread::onThreadLaunched() {
        // Do nothing.
    }

    bool Thread::shouldRecordInThreadTable() {
        return true;
    }

    long Thread::getEetop() const {
        return (long) this->_native_thread->native_handle();
    }

    Thread::~Thread() = default;

    JavaThread::JavaThread(Method *method, const std::list<oop> &args)
        : Thread(method, args) {
    }

    void JavaThread::start() {
        // No other threads will join this thread.
        // So it is OK to detach()
        this->_native_thread->detach();

        // A thread must start with an empty frame
        assert(_frames.getSize() == 0);

        // Only one argument(this) in java.lang.Thread#run()
        assert(_args.size() == 1);

        runMethod(_method, _args);

        Threads::threadStateChangeLock().lock();
        this->setThreadState(ThreadState::DIED);
        Threads::threadStateChangeLock().unlock();

        if (this->shouldRecordInThreadTable()) {
            Threads::decAppThreadCountLocked();
        }
    }

    oop JavaThread::runMethod(Method *method, const std::list<oop> &args) {
        D("### JavaThread::runMethod(), maxLocals: %d, maxStack: %d", method->getMaxLocals(), method->getMaxStack());
        Frame frame(method->getMaxLocals(), method->getMaxStack());
        Locals &locals = frame.getLocals();
        D("### Stack is at %p, locals is at %p", &frame.getStack(), &locals);

        // copy args to local variable table
        int localVariableIndex = 0;
        const std::vector<ValueType> descriptorMap = method->getArgumentValueTypes();

        D("Copying arguments to local variable table");
        std::for_each(args.begin(), args.end(), [&](oop arg) {
            if (arg == nullptr) {
                D("Copying null");
                locals.setReference(localVariableIndex++, nullptr);
                return;
            }

            switch (arg->getMarkOop()->getOopType()) {
                case oopType::INSTANCE_OOP:
                case oopType::OBJECT_ARRAY_OOP:
                case oopType::TYPE_ARRAY_OOP:
                    D("Copying reference: %p", arg);
                    locals.setReference(localVariableIndex++, arg);
                    break;

                case oopType::PRIMITIVE_OOP: {
                    switch (descriptorMap[localVariableIndex]) {
                        case ValueType::INT:
                            D("Copying int");
                            locals.setInt(localVariableIndex++, ((intOop) arg)->getValue());
                            break;
                        case ValueType::FLOAT:
                            D("Copying float");
                            locals.setFloat(localVariableIndex++, ((floatOop) arg)->getValue());
                            break;
                        case ValueType::DOUBLE:
                            D("Copying double");
                            locals.setDouble(localVariableIndex++, ((doubleOop) arg)->getValue());
                            break;
                        case ValueType::LONG:
                            D("Copying long");
                            locals.setLong(localVariableIndex++, ((longOop) arg)->getValue());
                            break;
                        default:
                            PANIC("Unknown value type");
                            break;
                    }
                }

                default:
                    PANIC("Unknown oop type");
            }
        });

        // give them to interpreter
        frame.setMethod(method);
        frame.setReturnPc(this->_pc);
        frame.setNativeFrame(method->isNative());

        this->_frames.push(&frame);
        this->_pc = 0;
        oop result = ByteCodeInterpreter::interp(this);
        this->_frames.pop();

        this->_pc = frame.getReturnPc();
        return result;
    }
}
