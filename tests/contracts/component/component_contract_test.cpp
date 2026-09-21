// Contract suite for the portable component object model.
// Specification: .refactor/COMPONENT_CONTRACT.md revision component-r1,
// section "Acceptance for COMPONENT-BASE".
//
// Every case here is one the contract names. Each was validated by temporarily
// inverting the behaviour it protects and confirming this suite fails; the
// inversions are listed in the worker report, not re-run here.
//
// Qt appears in the TEST only. Nothing in src/core/component/ includes or links
// Qt, and tests/architecture proves that separately once the target is
// registered.

#include <QtTest>

#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <type_traits>
#include <vector>

#include "test_objects.h"

using clashqt::com::ComPtr;
using clashqt::com::FormatInterfaceId;
using clashqt::com::IBuffer;
using clashqt::com::IComponentModule;
using clashqt::com::IErrorInfo;
using clashqt::com::InterfaceId;
using clashqt::com::InterfaceIdOf;
using clashqt::com::IObject;
using clashqt::com::IsFailure;
using clashqt::com::IsSuccess;
using clashqt::com::MakeInterfaceId;
using clashqt::com::Result;

using componenttest::ByteBuffer;
using componenttest::DestructionWitness;
using componenttest::DualObject;
using componenttest::PlainObject;
using componenttest::TestModule;

namespace {

// An id no object in this suite implements.
constexpr InterfaceId kUnimplementedId = MakeInterfaceId("11112222-3333-4444-5555-666677778888");

// A recognisable non-null value, so "the output was nulled" is distinguishable
// from "the output was never touched".
void* Poison() noexcept { return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x0D15EA5E)); }

}  // namespace

class ComponentContractTest : public QObject {
    Q_OBJECT

  private slots:
    // ---------------------------------------------------------------- Result

    void resultIsSignedSoFailureIsDetectable() {
        // The reference declares its status type unsigned and then tests
        // `< 0`, which is never true. This is that defect, asserted away.
        static_assert(static_cast<Result>(-1) < 0, "Result must be signed");
        QVERIFY(std::is_signed_v<Result>);
        QCOMPARE(sizeof(Result), sizeof(std::int32_t));
    }

    void everyResultCodeIsDistinct() {
        const QVector<QPair<const char*, Result>> codes = AllCodes();
        for (int i = 0; i < codes.size(); ++i) {
            for (int j = i + 1; j < codes.size(); ++j) {
                if (codes[i].second == codes[j].second) {
                    QFAIL(qPrintable(QStringLiteral("%1 and %2 share the value %3")
                                         .arg(QLatin1String(codes[i].first),
                                              QLatin1String(codes[j].first))
                                         .arg(codes[i].second)));
                }
            }
        }
        QCOMPARE(static_cast<int>(codes.size()), 12);
    }

    void resultCodesHoldTheirPublishedValues() {
        QCOMPARE(clashqt::com::kOk, 0);
        QCOMPARE(clashqt::com::kFalse, 1);
        QCOMPARE(clashqt::com::kFail, -1);
        QCOMPARE(clashqt::com::kNoInterface, -2);
        QCOMPARE(clashqt::com::kNotImplemented, -3);
        QCOMPARE(clashqt::com::kInvalidArgument, -4);
        QCOMPARE(clashqt::com::kNotFound, -5);
        QCOMPARE(clashqt::com::kTimeout, -6);
        QCOMPARE(clashqt::com::kCancelled, -7);
        QCOMPARE(clashqt::com::kUnsupportedVersion, -8);
        QCOMPARE(clashqt::com::kInvalidState, -9);
        QCOMPARE(clashqt::com::kAlreadyClosed, -10);
    }

    void isFailureIsTrueForEveryNegativeCode() {
        for (const auto& [name, code] : AllCodes()) {
            if (code < 0) {
                QVERIFY2(IsFailure(code), name);
                QVERIFY2(!IsSuccess(code), name);
            } else {
                QVERIFY2(IsSuccess(code), name);
                QVERIFY2(!IsFailure(code), name);
            }
        }
        // Not only the named codes: every negative value is a failure.
        QVERIFY(IsFailure(-1));
        QVERIFY(IsFailure(-12345));
        QVERIFY(IsFailure(std::numeric_limits<Result>::min()));
        QVERIFY(IsSuccess(0));
        QVERIFY(IsSuccess(std::numeric_limits<Result>::max()));
    }

    void aSuccessfulNoIsNotAFailure() {
        QVERIFY(IsSuccess(clashqt::com::kFalse));
        QVERIFY(clashqt::com::kFalse != clashqt::com::kOk);
    }

    // ----------------------------------------------------------- InterfaceId

    void publishedIdsMatchTheContractTableAndAreDistinct() {
        // Written out from .refactor/COMPONENT_CONTRACT.md, including the two
        // reserved ids that r1 does not implement, so a collision cannot be
        // introduced later without this failing.
        const QVector<QPair<const char*, InterfaceId>> table = {
            {"IObject", MakeInterfaceId("247a1b90-ece9-43a9-ab82-5739bdff6445")},
            {"IComponentModule", MakeInterfaceId("66fdef80-2cd3-4808-ac33-547172c2952f")},
            {"IErrorInfo", MakeInterfaceId("e83b4037-e096-4ae8-8f94-0f56ad29568a")},
            {"IBuffer", MakeInterfaceId("8fdacf5e-be9a-47e0-bb57-1375a2322e54")},
            {"IWeakReference (reserved)",
             MakeInterfaceId("5fd359f7-579a-4bcf-9743-cbbb8c5da432")},
            {"IWeakSource (reserved)", MakeInterfaceId("558f7604-facb-4122-86a8-16f66d58eac2")},
        };
        for (int i = 0; i < table.size(); ++i) {
            for (int j = i + 1; j < table.size(); ++j) {
                QVERIFY2(table[i].second != table[j].second, table[i].first);
            }
        }
        QVERIFY(InterfaceIdOf<IObject>() == table[0].second);
        QVERIFY(InterfaceIdOf<IComponentModule>() == table[1].second);
        QVERIFY(InterfaceIdOf<IErrorInfo>() == table[2].second);
        QVERIFY(InterfaceIdOf<IBuffer>() == table[3].second);
    }

    void theBaseInterfaceIdIsNotAllZeroes() {
        // The reference gives its base interface the nil id, which collides with
        // a default-constructed value and with every other nil id in the process.
        const InterfaceId nil = MakeInterfaceId("00000000-0000-0000-0000-000000000000");
        QVERIFY(InterfaceIdOf<IObject>() != nil);
        QVERIFY(InterfaceIdOf<IBuffer>() != nil);
        QVERIFY(InterfaceIdOf<IErrorInfo>() != nil);
        QVERIFY(InterfaceIdOf<IComponentModule>() != nil);
    }

    void idsFormatBackToTheirCanonicalText() {
        char text[clashqt::com::kInterfaceIdTextSize] = {};
        FormatInterfaceId(InterfaceIdOf<IObject>(), text);
        QCOMPARE(QLatin1String(text), QLatin1String("247a1b90-ece9-43a9-ab82-5739bdff6445"));
        FormatInterfaceId(InterfaceIdOf<IBuffer>(), text);
        QCOMPARE(QLatin1String(text), QLatin1String("8fdacf5e-be9a-47e0-bb57-1375a2322e54"));
    }

    // --------------------------------------------------- QueryInterface rules

    void nullOutParameterIsRejectedAndTheCountIsUntouched() {
        auto* object = new PlainObject();
        const std::int32_t before = object->PeekCount();
        QCOMPARE(object->QueryInterface(InterfaceIdOf<IObject>(), nullptr),
                 clashqt::com::kInvalidArgument);
        QCOMPARE(object->PeekCount(), before);
        // Also for an id that is not supported: the argument check comes first.
        QCOMPARE(object->QueryInterface(kUnimplementedId, nullptr),
                 clashqt::com::kInvalidArgument);
        QCOMPARE(object->PeekCount(), before);
        QCOMPARE(object->Release(), 0);
    }

    void unknownIdNullsTheOutputAndReturnsNoInterface() {
        auto* object = new PlainObject();
        const std::int32_t before = object->PeekCount();
        void* slot = Poison();
        const Result result = object->QueryInterface(kUnimplementedId, &slot);
        QCOMPARE(result, clashqt::com::kNoInterface);
        QVERIFY2(slot == nullptr,
                 "an unsupported query must null the output before it returns, so a caller "
                 "that ignores the code cannot read an uninitialised pointer");
        QCOMPARE(object->PeekCount(), before);
        QCOMPARE(object->Release(), 0);
    }

    void failedQueryLeavesTheReferenceCountUnchanged() {
        auto* object = new DualObject();
        object->AddRef();
        object->AddRef();
        const std::int32_t before = object->PeekCount();
        QCOMPARE(before, 3);
        void* slot = Poison();
        QCOMPARE(object->QueryInterface(kUnimplementedId, &slot), clashqt::com::kNoInterface);
        QCOMPARE(object->PeekCount(), before);
        QCOMPARE(object->QueryInterface(kUnimplementedId, nullptr),
                 clashqt::com::kInvalidArgument);
        QCOMPARE(object->PeekCount(), before);
        QCOMPARE(object->Release(), 2);
        QCOMPARE(object->Release(), 1);
        QCOMPARE(object->Release(), 0);
    }

    void successfulQueryTakesExactlyOneReference() {
        auto* object = new PlainObject();
        QCOMPARE(object->PeekCount(), 1);
        void* slot = nullptr;
        QCOMPARE(object->QueryInterface(InterfaceIdOf<IObject>(), &slot), clashqt::com::kOk);
        QVERIFY(slot != nullptr);
        QCOMPARE(object->PeekCount(), 2);  // exactly one new strong reference

        // And once more, to show it is one per successful call and not a cache.
        void* second = nullptr;
        QCOMPARE(object->QueryInterface(InterfaceIdOf<IObject>(), &second), clashqt::com::kOk);
        QCOMPARE(object->PeekCount(), 3);

        static_cast<IObject*>(second)->Release();
        static_cast<IObject*>(slot)->Release();
        QCOMPARE(object->PeekCount(), 1);
        QCOMPARE(object->Release(), 0);
    }

    void queryIsReflexive() {
        auto* object = new DualObject();
        ComPtr<IObject> identity;
        QCOMPARE(object->QueryInterface(InterfaceIdOf<IObject>(), identity.PutVoid()),
                 clashqt::com::kOk);
        ComPtr<IObject> again;
        QCOMPARE(identity->QueryInterface(InterfaceIdOf<IObject>(), again.PutVoid()),
                 clashqt::com::kOk);
        QCOMPARE(again.Get(), identity.Get());

        ComPtr<IBuffer> buffer;
        QCOMPARE(object->QueryInterface(InterfaceIdOf<IBuffer>(), buffer.PutVoid()),
                 clashqt::com::kOk);
        ComPtr<IBuffer> bufferAgain;
        QCOMPARE(buffer.As(bufferAgain), clashqt::com::kOk);
        QCOMPARE(bufferAgain.Get(), buffer.Get());

        identity.Reset();
        again.Reset();
        buffer.Reset();
        bufferAgain.Reset();
        QCOMPARE(object->Release(), 0);
    }

    void identityIsTheSamePointerThroughEveryInterface() {
        DestructionWitness witness;
        auto* object = new DualObject(&witness);
        IObject* expected = object->Identity();

        ComPtr<IBuffer> buffer;
        QCOMPARE(object->QueryInterface(InterfaceIdOf<IBuffer>(), buffer.PutVoid()),
                 clashqt::com::kOk);
        ComPtr<IErrorInfo> error;
        QCOMPARE(object->QueryInterface(InterfaceIdOf<IErrorInfo>(), error.PutVoid()),
                 clashqt::com::kOk);

        // Multiple inheritance: the interface pointers legitimately differ.
        QVERIFY(static_cast<void*>(buffer.Get()) != static_cast<void*>(error.Get()));

        ComPtr<IObject> fromBuffer;
        QCOMPARE(buffer.As(fromBuffer), clashqt::com::kOk);
        ComPtr<IObject> fromError;
        QCOMPARE(error.As(fromError), clashqt::com::kOk);
        QCOMPARE(fromBuffer.Get(), expected);
        QCOMPARE(fromError.Get(), expected);
        QCOMPARE(fromBuffer.Get(), fromError.Get());

        buffer.Reset();
        error.Reset();
        fromBuffer.Reset();
        fromError.Reset();
        QCOMPARE(object->Release(), 0);
        QCOMPARE(witness.destroyed, 1);
    }

    void queryIsSymmetric() {
        auto* object = new DualObject();
        ComPtr<IBuffer> buffer;
        QCOMPARE(object->QueryInterface(InterfaceIdOf<IBuffer>(), buffer.PutVoid()),
                 clashqt::com::kOk);

        // A -> B -> A returns the original pointer, and A -> B -> IObject
        // returns the original identity.
        ComPtr<IErrorInfo> error;
        QCOMPARE(buffer.As(error), clashqt::com::kOk);
        ComPtr<IBuffer> back;
        QCOMPARE(error.As(back), clashqt::com::kOk);
        QCOMPARE(back.Get(), buffer.Get());

        ComPtr<IObject> identity;
        QCOMPARE(error.As(identity), clashqt::com::kOk);
        QCOMPARE(identity.Get(), object->Identity());

        buffer.Reset();
        error.Reset();
        back.Reset();
        identity.Reset();
        QCOMPARE(object->Release(), 0);
    }

    void queryOutcomesAreStableForTheObjectsLifetime() {
        auto* object = new DualObject();
        void* first = nullptr;
        QCOMPARE(object->QueryInterface(InterfaceIdOf<IErrorInfo>(), &first), clashqt::com::kOk);

        for (int i = 0; i < 64; ++i) {
            void* supported = nullptr;
            QCOMPARE(object->QueryInterface(InterfaceIdOf<IErrorInfo>(), &supported),
                     clashqt::com::kOk);
            QCOMPARE(supported, first);
            static_cast<IErrorInfo*>(supported)->Release();

            void* unsupported = Poison();
            QCOMPARE(object->QueryInterface(kUnimplementedId, &unsupported),
                     clashqt::com::kNoInterface);
            QCOMPARE(unsupported, static_cast<void*>(nullptr));
        }
        QCOMPARE(object->PeekCount(), 2);
        static_cast<IErrorInfo*>(first)->Release();
        QCOMPARE(object->Release(), 0);
    }

    // --------------------------------------------------------------- ComPtr

    void adoptTakesOwnershipAndRetainAddsAReference() {
        auto* object = new PlainObject();
        QCOMPARE(object->PeekCount(), 1);
        {
            // Adopt: the reference the constructor already produced.
            auto adopted = ComPtr<PlainObject>::Adopt(object);
            QCOMPARE(object->PeekCount(), 1);
            {
                // Retain: a borrowed pointer gets a reference of its own.
                auto retained = ComPtr<PlainObject>::Retain(object);
                QCOMPARE(object->PeekCount(), 2);
            }
            QCOMPARE(object->PeekCount(), 1);
        }
        // Adopt's scope ended, so the object is gone; nothing is asserted about
        // it after this point.
    }

    void adoptedQueryOutputIsNotDoubleCounted() {
        auto* object = new PlainObject();
        ComPtr<IObject> queried;
        QCOMPARE(object->QueryInterface(InterfaceIdOf<IObject>(), queried.PutVoid()),
                 clashqt::com::kOk);
        QCOMPARE(object->PeekCount(), 2);  // the query's reference, adopted by Put
        queried.Reset();
        QCOMPARE(object->PeekCount(), 1);
        QCOMPARE(object->Release(), 0);
    }

    void copyRetainsAndDestructionReleases() {
        DestructionWitness witness;
        auto* object = new PlainObject(&witness);
        {
            auto first = ComPtr<PlainObject>::Adopt(object);
            QCOMPARE(object->PeekCount(), 1);
            auto second = first;  // copy retains
            QCOMPARE(object->PeekCount(), 2);
            {
                ComPtr<PlainObject> third;
                third = second;  // copy assignment retains
                QCOMPARE(object->PeekCount(), 3);
            }
            QCOMPARE(object->PeekCount(), 2);
            QCOMPARE(witness.destroyed, 0);
        }
        QCOMPARE(witness.destroyed, 1);
    }

    void selfAssignmentKeepsTheObjectAlive() {
        DestructionWitness witness;
        auto* object = new PlainObject(&witness);
        auto owner = ComPtr<PlainObject>::Adopt(object);

        ComPtr<PlainObject>& alias = owner;
        owner = alias;  // copy self-assignment
        QCOMPARE(witness.destroyed, 0);
        QCOMPARE(owner.Get(), object);
        QCOMPARE(object->PeekCount(), 1);
        // Still a live object, not freed memory that happens to read back.
        QCOMPARE(owner->QueryInterface(InterfaceIdOf<IObject>(), nullptr),
                 clashqt::com::kInvalidArgument);

        owner = std::move(alias);  // move self-assignment
        QCOMPARE(witness.destroyed, 0);
        QCOMPARE(owner.Get(), object);
        QCOMPARE(object->PeekCount(), 1);

        owner.Reset();
        QCOMPARE(witness.destroyed, 1);
    }

    void moveTransfersOwnershipWithoutChangingTheCount() {
        DestructionWitness witness;
        auto* object = new PlainObject(&witness);
        auto source = ComPtr<PlainObject>::Adopt(object);
        QCOMPARE(object->PeekCount(), 1);

        ComPtr<PlainObject> moved(std::move(source));
        QCOMPARE(object->PeekCount(), 1);  // transferred, not retained
        QVERIFY(source.Get() == nullptr);  // and the source no longer owns it
        QCOMPARE(moved.Get(), object);

        ComPtr<PlainObject> assigned;
        assigned = std::move(moved);
        QCOMPARE(object->PeekCount(), 1);
        QVERIFY(moved.Get() == nullptr);
        QCOMPARE(witness.destroyed, 0);

        assigned.Reset();
        QCOMPARE(witness.destroyed, 1);
    }

    void putReleasesThePreviousValueSoNothingLeaks() {
        DestructionWitness firstWitness;
        DestructionWitness secondWitness;
        auto* first = new PlainObject(&firstWitness);
        auto* second = new PlainObject(&secondWitness);

        auto owner = ComPtr<PlainObject>::Adopt(first);
        QCOMPARE(firstWitness.destroyed, 0);

        // Overwriting through the output parameter must not strand the old one.
        *owner.Put() = second;
        QCOMPARE(firstWitness.destroyed, 1);
        QCOMPARE(owner.Get(), second);
        QCOMPARE(second->PeekCount(), 1);

        // GetAddressOf is the same operation under the other name.
        auto* third = new PlainObject();
        *owner.GetAddressOf() = third;
        QCOMPARE(secondWitness.destroyed, 1);
        QCOMPARE(owner.Get(), third);

        owner.Reset();
    }

    void putIsSafeOnAnEmptyPointer() {
        ComPtr<PlainObject> owner;
        QVERIFY(*owner.Put() == nullptr);
        QVERIFY(owner.Get() == nullptr);
    }

    void detachYieldsOwnershipToTheCaller() {
        DestructionWitness witness;
        auto* object = new PlainObject(&witness);
        auto owner = ComPtr<PlainObject>::Adopt(object);
        PlainObject* raw = owner.Detach();
        QCOMPARE(raw, object);
        QVERIFY(owner.Get() == nullptr);
        QCOMPARE(witness.destroyed, 0);  // the scope did not release it
        QCOMPARE(raw->PeekCount(), 1);
        QCOMPARE(raw->Release(), 0);
        QCOMPARE(witness.destroyed, 1);
    }

    void asClearsTheOutputOnFailure() {
        auto* object = new PlainObject();
        auto owner = ComPtr<PlainObject>::Adopt(object);
        auto* survivor = new ByteBuffer("keep me until As overwrites it");
        ComPtr<IBuffer> out = ComPtr<IBuffer>::Adopt(survivor);

        // PlainObject does not implement IBuffer.
        QCOMPARE(owner.As(out), clashqt::com::kNoInterface);
        QVERIFY(out.Get() == nullptr);
        QCOMPARE(object->PeekCount(), 1);

        ComPtr<PlainObject> empty;
        ComPtr<IObject> fromEmpty;
        QCOMPARE(empty.As(fromEmpty), clashqt::com::kInvalidArgument);
        QVERIFY(fromEmpty.Get() == nullptr);
    }

    // ------------------------------------------------------------- lifetime

    void objectIsDestroyedWhenTheLastReferenceIsReleased() {
        DestructionWitness witness;
        auto* object = new PlainObject(&witness);
        QCOMPARE(object->AddRef(), 2);
        QCOMPARE(object->AddRef(), 3);
        QCOMPARE(object->Release(), 2);
        QCOMPARE(witness.destroyed, 0);
        QCOMPARE(object->Release(), 1);
        QCOMPARE(witness.destroyed, 0);
        QCOMPARE(object->Release(), 0);  // only a returned zero is reliable
        QCOMPARE(witness.destroyed, 1);  // and it means the object is gone
    }

    void referenceCountingIsAtomicUnderConcurrency() {
        // Atomic refcounting, which is a different claim from method
        // thread-safety: only AddRef/Release run concurrently here.
        DestructionWitness witness;
        auto* object = new PlainObject(&witness);
        constexpr int kThreads = 8;
        constexpr int kRounds = 20000;
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([object] {
                for (int i = 0; i < kRounds; ++i) {
                    object->AddRef();
                    object->Release();
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        QCOMPARE(object->PeekCount(), 1);
        QCOMPARE(witness.destroyed, 0);
        QCOMPARE(object->Release(), 0);
        QCOMPARE(witness.destroyed, 1);
    }

    // ----------------------------------------------- the declared interfaces

    void bufferOwnsItsBytesAndItsResizeCanFail() {
        auto buffer = ComPtr<IBuffer>::Adopt(new ByteBuffer("abc"));
        QCOMPARE(buffer->Size(), std::size_t{3});
        QCOMPARE(std::memcmp(buffer->Data(), "abc", 3), 0);
        QVERIFY(buffer->MutableData() != nullptr);
        QCOMPARE(buffer->Resize(2), clashqt::com::kOk);
        QCOMPARE(buffer->Size(), std::size_t{2});
        QCOMPARE(buffer->Resize(ByteBuffer::kMaximumSize + 1), clashqt::com::kFail);
        QCOMPARE(buffer->Size(), std::size_t{2});  // a failed resize changes nothing
    }

    void errorInfoIsRetrievedFromTheFailingObject() {
        auto* object = new DualObject();
        auto owner = ComPtr<IObject>::Adopt(object->Identity());

        ComPtr<IErrorInfo> info;
        QCOMPARE(owner.As(info), clashqt::com::kOk);
        Result code = clashqt::com::kOk;
        QCOMPARE(info->GetCode(&code), clashqt::com::kOk);
        QVERIFY(IsFailure(code));
        QCOMPARE(code, clashqt::com::kTimeout);
        QCOMPARE(info->GetCode(nullptr), clashqt::com::kInvalidArgument);

        ComPtr<IBuffer> message;
        QCOMPARE(info->GetMessage(message.Put()), clashqt::com::kOk);
        QCOMPARE(message->Size(), std::strlen("bounded wait expired"));

        // Absence of a part of the diagnostic is normal and distinguishable.
        ComPtr<IBuffer> source;
        QCOMPARE(info->GetSource(source.Put()), clashqt::com::kNotFound);
        QVERIFY(source.Get() == nullptr);

        // And an object that carries no diagnostic at all answers kNoInterface,
        // which is a different code again.
        auto plain = ComPtr<IObject>::Adopt(static_cast<IObject*>(new PlainObject()));
        ComPtr<IErrorInfo> none;
        QCOMPARE(plain.As(none), clashqt::com::kNoInterface);
        QVERIFY(none.Get() == nullptr);
    }

    void moduleDistinguishesAnUnknownClassFromAnUnknownInterface() {
        auto module = ComPtr<IComponentModule>::Adopt(new TestModule());
        QCOMPARE(module->AbiVersion(), 1u);
        clashqt::com::ModuleId id{};
        QCOMPARE(module->GetModuleId(&id), clashqt::com::kOk);
        QVERIFY(id == componenttest::kTestModuleId);
        QCOMPARE(module->GetModuleId(nullptr), clashqt::com::kInvalidArgument);
        QVERIFY(module->Description() != nullptr);

        ComPtr<IBuffer> buffer;
        QCOMPARE(module->CreateObject(componenttest::kByteBufferClassId,
                                      InterfaceIdOf<IBuffer>(), buffer.PutVoid()),
                 clashqt::com::kOk);
        QVERIFY(buffer.Get() != nullptr);

        // Unknown class: kNotFound. Known class, unknown interface:
        // kNoInterface. The reference gives these two conditions the same code.
        void* slot = Poison();
        QCOMPARE(module->CreateObject(componenttest::kUnknownClassId, InterfaceIdOf<IBuffer>(),
                                      &slot),
                 clashqt::com::kNotFound);
        QCOMPARE(slot, static_cast<void*>(nullptr));

        slot = Poison();
        QCOMPARE(module->CreateObject(componenttest::kPlainObjectClassId,
                                      InterfaceIdOf<IBuffer>(), &slot),
                 clashqt::com::kNoInterface);
        QCOMPARE(slot, static_cast<void*>(nullptr));

        QCOMPARE(module->CreateObject(componenttest::kPlainObjectClassId,
                                      InterfaceIdOf<IObject>(), nullptr),
                 clashqt::com::kInvalidArgument);
    }

  private:
    static QVector<QPair<const char*, Result>> AllCodes() {
        return {
            {"kOk", clashqt::com::kOk},
            {"kFalse", clashqt::com::kFalse},
            {"kFail", clashqt::com::kFail},
            {"kNoInterface", clashqt::com::kNoInterface},
            {"kNotImplemented", clashqt::com::kNotImplemented},
            {"kInvalidArgument", clashqt::com::kInvalidArgument},
            {"kNotFound", clashqt::com::kNotFound},
            {"kTimeout", clashqt::com::kTimeout},
            {"kCancelled", clashqt::com::kCancelled},
            {"kUnsupportedVersion", clashqt::com::kUnsupportedVersion},
            {"kInvalidState", clashqt::com::kInvalidState},
            {"kAlreadyClosed", clashqt::com::kAlreadyClosed},
        };
    }
};

QTEST_APPLESS_MAIN(ComponentContractTest)
#include "component_contract_test.moc"
