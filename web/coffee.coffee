$(document).ready ->
  class AppView extends Backbone.View

#    events:
#      "keypress #new-todo": "createOnEnter"

#    createOnEnter: (event) ->
#      return if event.keyCode != 13
#      Todos.create content: @input.val()
#      $('#new-todo').val ''

    initialize: ->
      #@input = @$('#new-todo')
      Disks.bind 'add', @addOne
      Disks.bind 'refresh', @addAll

      Disks.fetch()

    addOne: (disk) =>
      view = new DiskView(model: disk)
      @$('#disk-list').append(view.render())

    addAll: =>
      Disks.each @addOne

  class Disk extends Backbone.Model

  class DiskList extends Backbone.Collection
    model: Disk
    url: '/api/disks'

  window.Disks = new DiskList

  class DiskView extends Backbone.View
    tagName: 'li'

    render: ->
      content = @model.get('dev')
      $(@el).html(content)
      @el

  App = new AppView(el: $('#content'))
