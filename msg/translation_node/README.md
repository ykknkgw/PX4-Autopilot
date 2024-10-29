# Message Translations

This package contains a message translation node and a set of old message conversion methods.
It allows to run applications that are compiled with one set of message versions against a PX4 with another set of message versions, without having to change either the application or the PX4 side.

Specifically for this to work, topic publication/subscription/service names contain a message version in the form of `<topic_name>_v<version>`.

The translation node knows about all existing message versions, and dynamically monitors the publications, subscriptions and services, and then creates translations as needed.

## Updating a Message
When changing a message, a new version needs to be added.
The steps include:
- Copy the message file (`.msg`/`.srv`) to `px4_msgs_old/msg` (or `px4_msgs_old/srv`) and add the current version to the file name.
  For example `msg/VehicleAttitude.msg` becomes `px4_msgs_old/msg/VehicleAttitudeV3.msg`.
  Update the existing translations that use the current topic version to the now old version.
  For example `px4_msgs::msg::VehicleAttitude` becomes `px4_msgs_old::msg::VehicleAttitudeV3`.
- Increment `MESSAGE_VERSION` and update the message fields as desired.
- Add a version translation by adding a new translation header. Examples:
  - [`translations/example_translation_direct_v1.h`](translations/example_translation_direct_v1.h)
  - [`translations/example_translation_multi_v2.h`](translations/example_translation_multi_v2.h)
  - [`translations/example_translation_service_v1.h`](translations/example_translation_service_v1.h)
- Include the added header in [`translations/all_translations.h`](translations/all_translations.h).

For the second last step and for topics, there are two options:
- Direct translations: these translate a single topic between two different versions. This is the simpler case and should be preferred if possible.
- Generic case: this allows a translation between N input topics and M output topics.
  This can be used for merging or splitting a message.
  Or for example when moving a field from one message to another, a single translation should be added with the two older message versions as input and the two newer versions as output.
  This way there is no information lost when translating forward or backward.

Note: if a nested message definition changes, all messages including that message also require a version update.
This is primarily important for services.

## Usage in ROS
The message version can be added generically to a topic like this:
```c++
topic_name + "_v" + std::to_string(T::MESSAGE_VERSION)
```
Where `T` is the message type, e.g. `px4_msgs::msg::VehicleAttitude`.

The DDS client in PX4 automatically adds the version suffix if a message contains the field `uint32 MESSAGE_VERSION = x`.

Note: version 0 of a topic means not to add any `_v<version>` suffix.

## Implementation Details
The translation node dynamically monitors the topics and services.
It then instantiates the counter sides of the publications and subscribers as required.
For example if there is an external publisher for version 1 of a topic and subscriber for version 2.

Internally, it maintains a graph of all known topic and version tuples (which are the graph nodes).
The graph is connected by the message translations.
As arbitrary message translations can be registered, the graph can have cycles and multiple paths from one node to another.
Therefore on a topic update, the graph is traversed using a shortest path algorithm.
When moving from one node to the next, the message translation method is called with the current topic data.
If a node contains an instantiated publisher (because it previously detected an external subscriber), the data is published.
Thus, multiple subscribers of any version of the topic can be updated with the correct version of the data.

For translations with multiple input topics, the translation continues once all input messages are available.

## Limitations
- The current implementation depends on a service API that is not yet available in ROS Humble, and therefore does not support services when built for ROS Humble.
- Services only support a linear history, i.e. no message splitting or merging
- Having both publishers and subscribers for two different versions of a topic is currently not handled by the translation node and would trigger infinite circular publications.
  This could be extended if required.

Original document with requirements: https://docs.google.com/document/d/18_RxV1eEjt4haaa5QkFZAlIAJNv9w5HED2aUEiG7PVQ/edit?usp=sharing
